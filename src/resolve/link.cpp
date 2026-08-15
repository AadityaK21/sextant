#include "link.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fuse.h"
#include "record.h"

namespace sextant::resolve {
namespace {

namespace onto = sextant::ontology;

// Entities are decoded once and kept, because a Voyage's arrival timestamp is
// needed for every time-indexed edge it has and decoding it per edge would
// dominate the pass.
class EntityCache {
 public:
  EntityCache(codec::Store* store) : store_(store) {}

  const ResolvedEntity* Get(codec::TypeId type, const codec::Ulid& id) {
    const auto key = std::make_pair(type, id.ToString());
    const auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.valid ? &it->second.entity : nullptr;

    Entry entry;
    std::string payload;
    if (store_->GetEntity(type, id, &payload).ok()) {
      Slice slice(payload);
      entry.valid = ResolvedEntity::DecodeFrom(&slice, &entry.entity);
      entry.entity.id = id;
    }
    const auto inserted = cache_.emplace(key, std::move(entry));
    return inserted.first->second.valid ? &inserted.first->second.entity : nullptr;
  }

 private:
  struct Entry {
    ResolvedEntity entity;
    bool valid = false;
  };
  codec::Store* store_;
  std::map<std::pair<codec::TypeId, std::string>, Entry> cache_;
};

}  // namespace

Status ResolveLinks(codec::Store* store, const ontology::SchemaBundle& bundle,
                    const ResolverProperties& props, LinkReport* report) {
  (void)props;
  EntityCache entities(store);

  for (const auto& spec : bundle.sources()) {
    auto it = store->ScanSourceRecords(spec.id);
    for (; it->Valid(); it->Next()) {
      Slice value = it->value();
      onto::SourceRecord record;
      if (!onto::SourceRecord::DecodeFrom(&value, &record)) continue;
      if (record.links.empty()) continue;

      // Which entity did this source row end up in? XREF answers exactly that,
      // and it is the reason XREF exists.
      codec::Ulid source_entity;
      if (!store->LookupCrossRef(record.source_id, record.natural_key_hash,
                                 &source_entity)
               .ok()) {
        report->orphaned += record.links.size();
        continue;
      }

      const ResolvedEntity* entity = entities.Get(record.type, source_entity);
      codec::EntityWriter writer = store->EditEntity(record.type, source_entity);
      bool any = false;

      for (const auto& link : record.links) {
        ++report->references_seen;

        const onto::LinkTypeDef* def = bundle.ontology().Link(link.link_type);
        if (def == nullptr) continue;

        // The secondary-index lookup that turns "NLRTM" into an entity id.
        // This is why a link's match property must be declared indexed: without
        // IDX this is a full scan of every Port, once per link.
        codec::Ulid target;
        bool found = false;
        auto lookup = store->LookupString(link.target_type, link.match_property,
                                          Slice(link.match_value));
        for (; lookup->Valid(); lookup->Next()) {
          if (codec::DecodeIndexKeyEntity(lookup->key(), &target)) {
            found = true;
            break;  // first match wins; resolution has already deduplicated
          }
        }
        if (!found) {
          // A reference to something that is not in the data. The Digitraffic
          // feed names ports outside its own port list, and a dangling edge is
          // worse than no edge.
          ++report->unresolved;
          continue;
        }

        // A time-indexed link needs the timestamp from the SOURCE entity - the
        // voyage's own arrival time - anchored on the TARGET, because the
        // question is "what arrived at this port in this window".
        bool timed = false;
        if (def->HasTimeIndex() && entity != nullptr) {
          const onto::TValue* when = entity->Property(def->time_index);
          if (when != nullptr && !when->IsNull() &&
              when->type() == onto::ValueType::kTimestamp) {
            writer.AddTimedLink(def->id, target, when->AsTimestamp());
            timed = true;
            ++report->time_indexed;
          }
        }
        if (!timed) writer.AddLink(def->id, target);

        ++report->edges_written;
        ++report->by_link_type[def->name];
        any = true;
      }

      if (any) {
        const Status s = writer.Commit();
        if (!s.ok()) return s;
      }
    }
    const Status s = it->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

}  // namespace sextant::resolve
