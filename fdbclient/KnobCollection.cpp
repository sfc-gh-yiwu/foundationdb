/*
 * KnobCollection.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#include "fdbclient/KnobCollection.h"

#define init(knob, value) initKnob(knob, value, #knob)

TestKnobs::TestKnobs() {
	initialize();
}

void TestKnobs::initialize() {
	init(TEST_LONG, 0);
	init(TEST_INT, 0);
	init(TEST_DOUBLE, 0.0);
	init(TEST_BOOL, false);
	init(TEST_STRING, "");
}

bool TestKnobs::operator==(TestKnobs const& rhs) const {
	return (TEST_LONG == rhs.TEST_LONG) && (TEST_INT == rhs.TEST_INT) && (TEST_DOUBLE == rhs.TEST_DOUBLE) &&
	       (TEST_BOOL == rhs.TEST_BOOL) && (TEST_STRING == rhs.TEST_STRING);
}

bool TestKnobs::operator!=(TestKnobs const& rhs) const {
	return !(*this == rhs);
}

std::unique_ptr<KnobCollection> g_knobs;
