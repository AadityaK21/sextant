// Turning the engine's values into JSON, in one place.
//
// WHY THIS IS A SEPARATE FILE FROM THE SERVER
//
// Serialisation is the part worth testing and the server is the part that is
// awkward to test. Splitting them means a test can assert on the exact JSON a
// query produces without binding a socket, and the route handlers stay short
// enough to read in one screen.
//
// TIMESTAMPS GO OUT AS ISO 8601 STRINGS, NOT EPOCH MILLISECONDS
//
// The engine stores them as int64 milliseconds because that is what sorts and
// what arithmetic works on. The wire format is a string because a JSON number
// large enough to be an epoch in milliseconds is past 2^53 in some languages
// and silently loses precision - JavaScript being exactly the language on the
// other end of this API. Sending "2026-04-14T08:15:00Z" costs a few bytes and
// removes a whole class of bug that would show up as a timestamp being wrong
// by a fraction of a second in the browser and nowhere else.
//
// EVERY RESPONSE CARRIES _stats
//
// Including the ones where it is dull. A stats block that appears only on
// interesting responses is one the client has to write two code paths for, and
// it stops being something you can watch for regressions.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "execute.h"
#include "lineage.h"
#include "plan.h"
#include "schema.h"
#include "value.h"

namespace sextant::api {

using nlohmann::json;

// A property value. Timestamps become ISO strings; everything else maps onto
// the natural JSON type.
json ValueToJson(const ontology::TValue& value);

// The schema, which is what lets the frontend render types it was never
// compiled against.
json OntologyToJson(const ontology::Ontology& ontology);

json PlanToJson(const query::Plan& plan);
json CostToJson(const query::QueryCost& cost);
json EntityToJson(const query::ResultEntity& entity);
json ResultToJson(const query::QueryResult& result);

json ExplanationToJson(const lineage::Explanation& explanation);

// The shape every error goes out in, so the client has one thing to parse.
json ErrorToJson(const std::string& code, const std::string& message);

}  // namespace sextant::api
