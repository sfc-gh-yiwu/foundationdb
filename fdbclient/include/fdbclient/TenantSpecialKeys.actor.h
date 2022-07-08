/*
 * TenantSpecialKeys.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_TENANT_SPECIAL_KEYS_ACTOR_G_H)
#define FDBCLIENT_TENANT_SPECIAL_KEYS_ACTOR_G_H
#include "fdbclient/TenantSpecialKeys.actor.g.h"
#elif !defined(FDBCLIENT_TENANT_SPECIAL_KEYS_ACTOR_H)
#define FDBCLIENT_TENANT_SPECIAL_KEYS_ACTOR_H

#include "fdbclient/ActorLineageProfiler.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/SpecialKeySpace.actor.h"
#include "fdbclient/TenantManagement.actor.h"
#include "fdbclient/Tuple.h"
#include "flow/Arena.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // This must be the last #include.

template <bool HasSubRanges>
class TenantRangeImpl : public SpecialKeyRangeRWImpl {
private:
	static bool subRangeIntersects(KeyRangeRef subRange, KeyRangeRef range);

	static KeyRangeRef removePrefix(KeyRangeRef range, KeyRef prefix, KeyRef defaultEnd) {
		KeyRef begin = range.begin.removePrefix(prefix);
		KeyRef end;
		if (range.end.startsWith(prefix)) {
			end = range.end.removePrefix(prefix);
		} else {
			end = defaultEnd;
		}

		return KeyRangeRef(begin, end);
	}

	static KeyRef withTenantMapPrefix(KeyRef key, Arena& ar) {
		int keySize = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin.size() +
		              submoduleRange.begin.size() + mapSubRange.begin.size() + key.size();

		KeyRef prefixedKey = makeString(keySize, ar);
		uint8_t* mutableKey = mutateString(prefixedKey);

		mutableKey = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin.copyTo(mutableKey);
		mutableKey = submoduleRange.begin.copyTo(mutableKey);
		mutableKey = mapSubRange.begin.copyTo(mutableKey);

		key.copyTo(mutableKey);
		return prefixedKey;
	}

	ACTOR static Future<Void> getTenantList(ReadYourWritesTransaction* ryw,
	                                        KeyRangeRef kr,
	                                        RangeResult* results,
	                                        GetRangeLimits limitsHint) {
		std::map<TenantName, TenantMapEntry> tenants =
		    wait(TenantAPI::listTenantsTransaction(&ryw->getTransaction(), kr.begin, kr.end, limitsHint.rows));

		for (auto tenant : tenants) {
			json_spirit::mObject tenantEntry;
			tenantEntry["id"] = tenant.second.id;
			tenantEntry["prefix"] = tenant.second.prefix.toString();
			if (tenant.second.tenantGroup.present()) {
				tenantEntry["tenant_group"] = tenant.second.tenantGroup.get().toString();
			}
			std::string tenantEntryString = json_spirit::write_string(json_spirit::mValue(tenantEntry));
			ValueRef tenantEntryBytes(results->arena(), tenantEntryString);
			results->push_back(results->arena(),
			                   KeyValueRef(withTenantMapPrefix(tenant.first, results->arena()), tenantEntryBytes));
		}

		return Void();
	}

	ACTOR template <bool B>
	static Future<RangeResult> getTenantRange(ReadYourWritesTransaction* ryw,
	                                          KeyRangeRef kr,
	                                          GetRangeLimits limitsHint) {
		state RangeResult results;

		kr = kr.removePrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)
		         .removePrefix(TenantRangeImpl<B>::submoduleRange.begin);

		if (kr.intersects(TenantRangeImpl<B>::mapSubRange)) {
			GetRangeLimits limits = limitsHint;
			limits.decrement(results);
			wait(getTenantList(
			    ryw,
			    removePrefix(kr & TenantRangeImpl<B>::mapSubRange, TenantRangeImpl<B>::mapSubRange.begin, "\xff"_sr),
			    &results,
			    limits));
		}

		return results;
	}

	static void applyTenantConfig(ReadYourWritesTransaction* ryw,
	                              TenantNameRef tenantName,
	                              std::vector<std::pair<Standalone<StringRef>, Optional<Value>>> configEntries,
	                              TenantMapEntry* tenantEntry) {

		std::vector<std::pair<Standalone<StringRef>, Optional<Value>>>::iterator configItr;
		for (configItr = configEntries.begin(); configItr != configEntries.end(); ++configItr) {
			if (configItr->first == "tenant_group"_sr) {
				tenantEntry->tenantGroup = configItr->second;
			} else {
				TraceEvent(SevWarn, "InvalidTenantConfig")
				    .detail("TenantName", tenantName)
				    .detail("ConfigName", configItr->first);
				ryw->setSpecialKeySpaceErrorMsg(
				    ManagementAPIError::toJsonString(false,
				                                     "set tenant configuration",
				                                     format("invalid tenant configuration option `%s' for tenant `%s'",
				                                            configItr->first.toString().c_str(),
				                                            tenantName.toString().c_str())));
				throw special_keys_api_failure();
			}
		}
	}

	ACTOR static Future<Void> createTenant(
	    ReadYourWritesTransaction* ryw,
	    TenantNameRef tenantName,
	    Optional<std::vector<std::pair<Standalone<StringRef>, Optional<Value>>>> configMutations,
	    int64_t tenantId) {
		state TenantMapEntry tenantEntry;
		tenantEntry.id = tenantId;

		if (configMutations.present()) {
			applyTenantConfig(ryw, tenantName, configMutations.get(), &tenantEntry);
		}

		std::pair<TenantMapEntry, bool> entry =
		    wait(TenantAPI::createTenantTransaction(&ryw->getTransaction(), tenantName, tenantEntry));

		return Void();
	}

	ACTOR static Future<Void> createTenants(
	    ReadYourWritesTransaction* ryw,
	    std::map<TenantName, Optional<std::vector<std::pair<Standalone<StringRef>, Optional<Value>>>>> tenants) {
		Optional<Value> lastIdVal = wait(ryw->getTransaction().get(tenantLastIdKey));
		int64_t previousId = lastIdVal.present() ? TenantMapEntry::prefixToId(lastIdVal.get()) : -1;

		std::vector<Future<Void>> createFutures;
		for (auto const& [tenant, config] : tenants) {
			createFutures.push_back(createTenant(ryw, tenant, config, ++previousId));
		}

		ryw->getTransaction().set(tenantLastIdKey, TenantMapEntry::idToPrefix(previousId));
		wait(waitForAll(createFutures));
		return Void();
	}

	ACTOR static Future<Void> changeTenantConfig(
	    ReadYourWritesTransaction* ryw,
	    TenantName tenantName,
	    std::vector<std::pair<Standalone<StringRef>, Optional<Value>>> configEntries) {
		state TenantMapEntry tenantEntry = wait(TenantAPI::getTenantTransaction(&ryw->getTransaction(), tenantName));

		applyTenantConfig(ryw, tenantName, configEntries, &tenantEntry);
		TenantAPI::configureTenantTransaction(&ryw->getTransaction(), tenantName, tenantEntry);

		return Void();
	}

	ACTOR static Future<Void> deleteTenantRange(ReadYourWritesTransaction* ryw,
	                                            TenantName beginTenant,
	                                            TenantName endTenant) {
		state std::map<TenantName, TenantMapEntry> tenants = wait(
		    TenantAPI::listTenantsTransaction(&ryw->getTransaction(), beginTenant, endTenant, CLIENT_KNOBS->TOO_MANY));

		if (tenants.size() == CLIENT_KNOBS->TOO_MANY) {
			TraceEvent(SevWarn, "DeleteTenantRangeTooLange")
			    .detail("BeginTenant", beginTenant)
			    .detail("EndTenant", endTenant);
			ryw->setSpecialKeySpaceErrorMsg(
			    ManagementAPIError::toJsonString(false, "delete tenants", "too many tenants to range delete"));
			throw special_keys_api_failure();
		}

		std::vector<Future<Void>> deleteFutures;
		for (auto tenant : tenants) {
			deleteFutures.push_back(TenantAPI::deleteTenantTransaction(&ryw->getTransaction(), tenant.first));
		}
		wait(waitForAll(deleteFutures));

		return Void();
	}

public:
	// These ranges vary based on the template parameter
	const static KeyRangeRef submoduleRange;
	const static KeyRangeRef mapSubRange;

	// These sub-ranges should only be used if HasSubRanges=true
	const inline static KeyRangeRef configureSubRange = KeyRangeRef("configure/"_sr, "configure0"_sr);

	explicit TenantRangeImpl(KeyRangeRef kr) : SpecialKeyRangeRWImpl(kr) {}

	Future<RangeResult> getRange(ReadYourWritesTransaction* ryw,
	                             KeyRangeRef kr,
	                             GetRangeLimits limitsHint) const override {
		return getTenantRange<HasSubRanges>(ryw, kr, limitsHint);
	}

	Future<Optional<std::string>> commit(ReadYourWritesTransaction* ryw) override {
		auto ranges = ryw->getSpecialKeySpaceWriteMap().containedRanges(range);
		std::vector<Future<Void>> tenantManagementFutures;

		std::vector<std::pair<KeyRangeRef, Optional<Value>>> mapMutations;
		std::map<TenantName, std::vector<std::pair<Standalone<StringRef>, Optional<Value>>>> configMutations;

		for (auto range : ranges) {
			if (!range.value().first) {
				continue;
			}

			KeyRangeRef adjustedRange =
			    range.range()
			        .removePrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)
			        .removePrefix(submoduleRange.begin);

			if (subRangeIntersects(mapSubRange, adjustedRange)) {
				adjustedRange = mapSubRange & adjustedRange;
				adjustedRange = removePrefix(adjustedRange, mapSubRange.begin, "\xff"_sr);
				mapMutations.push_back(std::make_pair(adjustedRange, range.value().second));
			} else if (subRangeIntersects(configureSubRange, adjustedRange) && adjustedRange.singleKeyRange()) {
				StringRef configTupleStr = adjustedRange.begin.removePrefix(configureSubRange.begin);
				try {
					Tuple tuple = Tuple::unpack(configTupleStr);
					if (tuple.size() != 2) {
						throw invalid_tuple_index();
					}
					configMutations[tuple.getString(0)].push_back(
					    std::make_pair(tuple.getString(1), range.value().second));
				} catch (Error& e) {
					TraceEvent(SevWarn, "InvalidTenantConfigurationKey").error(e).detail("Key", adjustedRange.begin);
					ryw->setSpecialKeySpaceErrorMsg(ManagementAPIError::toJsonString(
					    false, "configure tenant", "invalid tenant configuration key"));
					throw special_keys_api_failure();
				}
			}
		}

		std::map<TenantName, Optional<std::vector<std::pair<Standalone<StringRef>, Optional<Value>>>>> tenantsToCreate;
		for (auto mapMutation : mapMutations) {
			TenantNameRef tenantName = mapMutation.first.begin;
			if (mapMutation.second.present()) {
				Optional<std::vector<std::pair<Standalone<StringRef>, Optional<Value>>>> createMutations;
				auto itr = configMutations.find(tenantName);
				if (itr != configMutations.end()) {
					createMutations = itr->second;
					configMutations.erase(itr);
				}
				tenantsToCreate[tenantName] = createMutations;
			} else {
				// For a single key clear, just issue the delete
				if (mapMutation.first.singleKeyRange()) {
					tenantManagementFutures.push_back(
					    TenantAPI::deleteTenantTransaction(&ryw->getTransaction(), tenantName));

					// Configuration changes made to a deleted tenant are discarded
					configMutations.erase(tenantName);
				} else {
					tenantManagementFutures.push_back(deleteTenantRange(ryw, tenantName, mapMutation.first.end));

					// Configuration changes made to a deleted tenant are discarded
					configMutations.erase(configMutations.lower_bound(tenantName),
					                      configMutations.lower_bound(mapMutation.first.end));
				}
			}
		}

		if (!tenantsToCreate.empty()) {
			tenantManagementFutures.push_back(createTenants(ryw, tenantsToCreate));
		}
		for (auto configMutation : configMutations) {
			tenantManagementFutures.push_back(changeTenantConfig(ryw, configMutation.first, configMutation.second));
		}

		return tag(waitForAll(tenantManagementFutures), Optional<std::string>());
	}
};

#include "flow/unactorcompiler.h"
#endif