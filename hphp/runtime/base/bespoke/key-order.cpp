
/*
  +----------------------------------------------------------------------+
  | HipHop for PHP                                                       |
  +----------------------------------------------------------------------+
  | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/bespoke/key-order.h"
#include "hphp/runtime/base/string-data.h"

#include <sstream>

namespace HPHP { namespace bespoke {

TRACE_SET_MOD(bespoke);

namespace {

const StaticString s_extraKey("...");

struct KeyCollectionHash {
  size_t operator()(const KeyOrder::KeyOrderData& ko) const {
    size_t seed = 0;
    for (auto const key : ko) {
      seed = folly::hash::hash_combine(seed, key->hash());
    }
    return seed;
  }
};

using KeyOrderSet =
  std::unordered_set<KeyOrder::KeyOrderData, KeyCollectionHash>;

KeyOrderSet s_keyOrderSet;
folly::SharedMutex s_keyOrderLock;

}

KeyOrder KeyOrder::insert(const StringData* k) const {
  if (!k->isStatic() || !m_keys) return KeyOrder::MakeInvalid();
  if (contains(k)) {
    return *this;
  }
  if (isTooLong()) return *this;
  KeyOrderData newOrder{*m_keys};
  auto const full = m_keys->size() == RO::EvalBespokeStructDictMaxNumKeys;
  newOrder.push_back(full ? s_extraKey.get() : k);
  return Make(newOrder);
}

KeyOrder KeyOrder::remove(const StringData* k) const {
  if (!valid()) return *this;
  KeyOrderData newOrder{*m_keys};
  newOrder.erase(std::remove_if(newOrder.begin(), newOrder.end(),
                                [&](LowStringPtr key) { return k->same(key); }),
                 newOrder.end());
  return Make(newOrder);
}

KeyOrder KeyOrder::pop() const {
  if (empty() || !valid()) return *this;
  KeyOrderData newOrder{*m_keys};
  newOrder.pop_back();
  return Make(newOrder);
}

KeyOrder KeyOrder::Make(const KeyOrderData& ko) {
  auto trimmedKeyOrder = trimKeyOrder(ko);
  {
    folly::SharedMutex::ReadHolder rlock{s_keyOrderLock};
    auto it = s_keyOrderSet.find(trimmedKeyOrder);
    if (it != s_keyOrderSet.end()) return KeyOrder{&*it};
  }

  folly::SharedMutex::WriteHolder wlock{s_keyOrderLock};
  auto const ret = s_keyOrderSet.insert(std::move(trimmedKeyOrder));
  return KeyOrder{&*ret.first};
}

KeyOrder::KeyOrder(const KeyOrderData* keys)
  : m_keys(keys)
{}

KeyOrder::KeyOrderData KeyOrder::trimKeyOrder(const KeyOrderData& ko) {
  KeyOrderData res{ko};
  if (res.size() > RO::EvalBespokeStructDictMaxNumKeys) {
    res.resize(RO::EvalBespokeStructDictMaxNumKeys);
    res.push_back(s_extraKey.get());
  }
  return res;
}

std::string KeyOrder::toString() const {
  if (!m_keys) return "<invalid>";
  std::stringstream ss;
  ss << '[';
  for (auto i = 0; i < m_keys->size(); ++i) {
    if (i > 0) ss << ',';
    ss << '"' << (*m_keys)[i]->data() << '"';
  }
  ss << ']';
  return ss.str();
}

bool KeyOrder::operator==(const KeyOrder& other) const {
  return this->m_keys == other.m_keys;
}

KeyOrder KeyOrder::ForArray(const ArrayData* ad) {
  KeyOrderData ko;
  auto hasStaticStrKeysOnly = true;
  IterateKV(ad, [&](auto k, auto /*v*/) -> bool {
    if (tvIsString(k) && val(k).pstr->isStatic()) {
      ko.push_back(val(k).pstr);
      return false;
    } else {
      hasStaticStrKeysOnly = false;
      return true;
    }
  });
  return hasStaticStrKeysOnly ? Make(ko) : KeyOrder();
}

KeyOrder KeyOrder::MakeInvalid() {
  return KeyOrder{};
}

bool KeyOrder::isTooLong() const {
  assertx(m_keys);
  return m_keys->size() > kMaxTrackedKeyOrderSize;
}

size_t KeyOrder::size() const {
  assertx(valid());
  return m_keys->size();
}

bool KeyOrder::empty() const {
  return m_keys && m_keys->empty();
}

bool KeyOrder::valid() const {
  return m_keys && !isTooLong();
}

bool KeyOrder::contains(const StringData* val) const {
  assertx(valid());
  return std::find(m_keys->begin(), m_keys->end(), val) != m_keys->end();
}

KeyOrder::const_iterator KeyOrder::begin() const {
  assertx(m_keys);
  return m_keys->begin();
}

KeyOrder::const_iterator KeyOrder::end() const {
  assertx(m_keys);
  return m_keys->end();
}

KeyOrder collectKeyOrder(const KeyOrderMap& keyOrderMap) {
  std::unordered_set<const StringData*> keys;
  for (auto const& pair : keyOrderMap) {
    if (!pair.first.valid()) return pair.first;
    keys.insert(pair.first.begin(), pair.first.end());
  }

  if (keys.size() > RO::EvalBespokeStructDictMaxNumKeys) {
    return KeyOrder::MakeInvalid();
  }

  KeyOrder::KeyOrderData sorted;
  for (auto const key : keys) {
    sorted.push_back(key);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](auto a, auto b) { return a->compare(b) < 0; });
  return KeyOrder::Make(sorted);
}

namespace {
using KeyCountMap = folly::F14FastMap<const StringData*, size_t>;
using KeyOrderFrequencyList = std::vector<std::pair<KeyOrder, size_t>>;
/*
 * Returns a map indicating, for each key in the KeyOrderMap, how many key
 * order instances would be invalidated by removing it.
 */
KeyCountMap countKeyInstances(const KeyOrderFrequencyList& frequencyList) {
  auto keyInstances = KeyCountMap{};
  for (auto const& [keyOrder, count] : frequencyList) {
    for (auto const& key : keyOrder) {
      keyInstances[key] += count;
    }
  }

  return keyInstances;
}
}

KeyOrder pruneKeyOrder(const KeyOrderMap& keyOrderMap, double cutoff) {
  auto workingSet = KeyOrderFrequencyList();
  workingSet.reserve(keyOrderMap.size());
  for (auto const& [keyOrder, count] : keyOrderMap) {
    workingSet.emplace_back(keyOrder, count);
  }

  auto const sumCounts = [](auto begin, auto end) {
    return std::accumulate(
      begin, end,
      size_t{0},
      [&](auto const& a, auto const& b) { return a + b.second; }
    );
  };

  // Compute the total number of key order instances we begin with.
  auto const total = sumCounts(workingSet.begin(), workingSet.end());

  // Immediately prune invalid key orders.
  auto accepted = total;
  {
    auto const removeAt = std::partition(
      workingSet.begin(), workingSet.end(),
      [&](auto const& a) { return a.first.valid(); }
    );
    accepted -= sumCounts(removeAt, workingSet.end());
    workingSet.erase(removeAt, workingSet.end());
    assertx(accepted == sumCounts(workingSet.begin(), workingSet.end()));
    FTRACE(2, "Prune invalid. Remain: {} / {}\n", accepted, total);
  }

  // If we're already below our cutoff, abort.
  if (accepted < total * cutoff) return KeyOrder::MakeInvalid();

  // Greedily remove the key which invalidates the least number of key order
  // instances. Stop when doing so again would cross the cutoff.
  auto keyInstances = countKeyInstances(workingSet);
  while (!keyInstances.empty()) {
    auto const keyAndCount = std::min_element(
      keyInstances.cbegin(), keyInstances.cend(),
      [](auto const& a, auto const& b) { return a.second < b.second; }
    );

    accepted -= keyAndCount->second;
    if (accepted < total * cutoff) break;

    FTRACE(2, "Prune key \"{}\". Remain: {} / {}\n", keyAndCount->first,
           accepted, total);

    // Remove all key orders invalidated by this removal, and update the
    // instance counts.
    auto removeAt = std::partition(
      workingSet.begin(), workingSet.end(),
      [&](auto const& a) { return !a.first.contains(keyAndCount->first); }
    );
    std::for_each(
      removeAt, workingSet.end(),
      [&](auto const& pair) {
        for (auto const& key : pair.first) {
          auto const iter = keyInstances.find(key);
          assertx(iter != keyInstances.end());
          iter->second -= pair.second;
          if (iter->second == 0) {
            keyInstances.erase(iter);
          }
        }
      }
    );

    workingSet.erase(removeAt, workingSet.end());

    assertx(accepted == sumCounts(workingSet.begin(), workingSet.end()));
    assertx(keyInstances == countKeyInstances(workingSet));
  }

  // Assemble the final pruned key order.
  auto prunedResult = KeyOrderMap();
  for (auto const& [keyOrder, count] : workingSet) {
    prunedResult.emplace(keyOrder, count);
  }

  auto const result = collectKeyOrder(prunedResult);
  return result;
}

void mergeKeyOrderMap(KeyOrderMap& dst, const KeyOrderMap& src) {
  for (auto const& [key, count] : src) {
    auto iter = dst.find(key);
    if (iter == dst.end()) {
      dst.emplace(key, count);
    } else {
      iter->second = iter->second + count;
    }
  }
}

}}
