#include "json.h"

#include <string>
#include <vector>

namespace sextant::api {

using ontology::ValueType;

json ValueToJson(const ontology::TValue& value) {
  switch (value.type()) {
    case ValueType::kNull:
      return nullptr;
    case ValueType::kString:
      return value.AsString();
    case ValueType::kInt:
      return value.AsInt();
    case ValueType::kDouble:
      return value.AsDouble();
    case ValueType::kBool:
      return value.AsBool();
    case ValueType::kTimestamp:
      // A string, deliberately. See the header: an epoch in milliseconds is
      // past the exact-integer range of a JavaScript number.
      return ontology::FormatIso8601(value.AsTimestamp());
    case ValueType::kStringList:
      return value.AsStringList();
  }
  return nullptr;
}

json OntologyToJson(const ontology::Ontology& ontology) {
  json types = json::array();
  for (const auto& type : ontology.types()) {
    json properties = json::array();
    for (const auto& prop : type.properties) {
      json p = {
          {"id", prop.id},
          {"name", prop.name},
          {"type", ontology::ValueTypeName(prop.type)},
          {"title", prop.title},
          {"indexed", prop.indexed},
          {"unique_hint", prop.unique_hint},
          {"fuse", ontology::FusionRuleName(prop.fuse)},
      };
      if (prop.IsEnum()) p["enum"] = prop.enum_values;
      properties.push_back(p);
    }
    types.push_back({
        {"id", type.id},
        {"name", type.name},
        {"description", type.description},
        {"display", type.display},
        {"properties", properties},
    });
  }

  json links = json::array();
  for (const auto& link : ontology.links()) {
    const auto* from = ontology.Type(link.from);
    const auto* to = ontology.Type(link.to);
    json l = {
        {"id", link.id},
        {"name", link.name},
        {"from", from != nullptr ? from->name : ""},
        {"to", to != nullptr ? to->name : ""},
        {"cardinality", ontology::CardinalityName(link.cardinality)},
        {"inverse", link.inverse},
        {"time_indexed", link.HasTimeIndex()},
    };
    // The frontend uses this to decide whether offering a date filter on this
    // link would produce a fast query or a slow one, and can say so in the UI.
    if (link.HasTimeIndex()) {
      const auto* prop = ontology.Property(link.time_index);
      l["time_index"] = prop != nullptr ? prop->name : "";
    }
    links.push_back(l);
  }

  return {{"version", ontology.version()},
          {"namespace", ontology.name_space()},
          {"types", types},
          {"links", links}};
}

json PlanToJson(const query::Plan& plan) {
  json steps = json::array();
  for (const auto& step : plan.steps) {
    json residuals = json::array();
    for (const auto& pred : step.residuals) {
      residuals.push_back({{"property", pred.property},
                           {"op", query::CompareOpName(pred.op)},
                           {"value", ValueToJson(pred.value)}});
    }
    json s = {
        {"ordinal", step.ordinal},
        {"description", step.description},
        {"access_path", query::AccessPathName(step.path)},
        {"keyspace", query::AccessPathKeyspace(step.path)},
        {"reason", step.reason},
        {"residuals", residuals},
    };
    if (!step.type.empty()) s["type"] = step.type;
    if (!step.link.empty()) s["link"] = step.link;
    if (!step.property.empty()) s["property"] = step.property;
    if (step.window.present) {
      s["window"] = {{"from", ontology::FormatIso8601(step.window.from_inclusive)},
                     {"to", ontology::FormatIso8601(step.window.to_exclusive)}};
    }
    steps.push_back(s);
  }
  return {{"steps", steps},
          {"warnings", plan.warnings},
          {"index_used", query::AccessPathName(plan.headline)}};
}

json CostToJson(const query::QueryCost& cost) {
  json out = {
      {"keys_scanned", cost.keys_scanned},
      {"blocks_read", cost.blocks_read},
      {"block_cache_hits", cost.block_cache_hits},
      {"bloom_rejections", cost.bloom_rejections},
      {"range_rejections", cost.range_rejections},
      {"sstables_probed", cost.sstables_probed},
      {"memtable_hits", cost.memtable_hits},
      {"entities_materialised", cost.entities_materialised},
      {"index_used", query::AccessPathName(cost.index_used)},
      {"elapsed_us", cost.elapsed_us},
  };
  // Only present when it happened, so its presence is the signal.
  if (cost.truncated) {
    out["truncated"] = true;
    out["truncated_at"] = cost.truncated_at;
  }
  return out;
}

json EntityToJson(const query::ResultEntity& entity) {
  json properties = json::object();
  for (const auto& [name, value] : entity.properties) {
    properties[name] = ValueToJson(value);
  }
  return {{"id", entity.id.ToString()},
          {"type", entity.type_name},
          {"display", entity.display},
          {"depth", entity.depth},
          {"properties", properties}};
}

json ResultToJson(const query::QueryResult& result) {
  json entities = json::array();
  for (const auto& entity : result.entities) entities.push_back(EntityToJson(entity));

  json edges = json::array();
  for (const auto& edge : result.edges) {
    edges.push_back({{"from", edge.from.ToString()},
                     {"to", edge.to.ToString()},
                     {"link", edge.link},
                     {"reverse", edge.reverse}});
  }

  return {{"entities", entities},
          {"edges", edges},
          {"count", result.entities.size()},
          {"total_before_limit", result.total_before_limit},
          {"_plan", PlanToJson(result.plan)},
          {"_stats", CostToJson(result.cost)}};
}

json ExplanationToJson(const lineage::Explanation& explanation) {
  json rejected = json::array();
  for (const auto& value : explanation.rejected) {
    rejected.push_back({{"source", value.source_id},
                        {"batch", value.batch_id},
                        {"row", value.row_seq},
                        {"column", value.column},
                        {"value", value.value},
                        {"reason", value.reason}});
  }

  json chain = json::array();
  for (size_t i = 0; i < explanation.transform_names.size(); ++i) {
    chain.push_back({{"id", i < explanation.chain.size() ? explanation.chain[i] : 0},
                     {"name", explanation.transform_names[i]}});
  }

  return {
      {"entity", explanation.entity_id.ToString()},
      {"property", explanation.property_name},
      {"stored_value", explanation.stored_value},
      {"found", explanation.provenance_found},
      {"origin",
       {{"source", explanation.source_key},
        {"batch", explanation.batch_id},
        {"row", explanation.row_seq},
        {"column", explanation.column}}},
      {"raw_row_found", explanation.raw_row_found},
      {"raw_row", explanation.raw_row},
      {"raw_cell", explanation.raw_cell},
      {"transforms", chain},
      {"chain_fingerprint", explanation.chain_fingerprint},
      {"chain_changed", explanation.chain_changed},
      {"fusion", {{"rule", explanation.rule}, {"confidence", explanation.confidence}}},
      {"rejected", rejected},
      {"cluster_size", explanation.cluster_size},
      {"merge_evidence", explanation.merge_evidence},
      // The round trip, per property. This is the field that makes the lineage
      // panel a proof rather than a display: it is recomputed on read, not
      // stored, so a stale or corrupted provenance record shows up here.
      {"replay",
       {{"value", explanation.replayed_value},
        {"matches", explanation.replay_matches},
        {"error", explanation.replay_error}}},
  };
}

json ErrorToJson(const std::string& code, const std::string& message) {
  return {{"error", {{"code", code}, {"message", message}}}};
}

}  // namespace sextant::api
