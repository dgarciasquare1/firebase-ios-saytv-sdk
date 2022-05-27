/*
 * Copyright 2019 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/src/local/leveldb_index_manager.h"
#include "Firestore/core/src/core/bound.h"
#include "Firestore/core/src/local/leveldb_persistence.h"
#include "Firestore/core/src/model/field_index.h"
#include "Firestore/core/test/unit/local/index_manager_test.h"
#include "Firestore/core/test/unit/local/persistence_testing.h"
#include "Firestore/core/test/unit/testutil/testutil.h"
#include "absl/memory/memory.h"
#include "gtest/gtest.h"

namespace firebase {
namespace firestore {
namespace local {

namespace {

using core::Bound;
using credentials::User;
using model::FieldIndex;
using model::IndexOffset;
using model::IndexState;
using model::ResourcePath;
using model::Segment;
using testutil::Array;
using testutil::CollectionGroupQuery;
using testutil::DeletedDoc;
using testutil::Doc;
using testutil::Filter;
using testutil::Key;
using testutil::MakeFieldIndex;
using testutil::Map;
using testutil::OrderBy;
using testutil::Query;
using testutil::Version;

std::unique_ptr<Persistence> PersistenceFactory() {
  return LevelDbPersistenceForTesting();
}

void VerifySequenceNumber(IndexManager* index_manager,
                          const std::string& group,
                          int32_t expected_seq_num) {
  std::vector<FieldIndex> indexes = index_manager->GetFieldIndexes(group);
  EXPECT_EQ(indexes.size(), 1);
  EXPECT_EQ(indexes[0].index_state().sequence_number(), expected_seq_num);
}

}  // namespace

INSTANTIATE_TEST_SUITE_P(LevelDbIndexManagerTest,
                         IndexManagerTest,
                         ::testing::Values(PersistenceFactory));

class LevelDbIndexManagerTest : public ::testing::Test {
 public:
  // `GetParam()` must return a factory function.
  LevelDbIndexManagerTest() : persistence{PersistenceFactory()} {
    index_manager = persistence->GetIndexManager(User::Unauthenticated());
  }

  void AddDocs(const std::vector<model::MutableDocument>& docs) {
    model::DocumentMap map;
    for (const auto& doc : docs) {
      map = map.insert(doc.key(), doc);
    }
    index_manager->UpdateIndexEntries(std::move(map));
  }

  void AddDoc(const std::string& key,
              nanopb::Message<google_firestore_v1_Value> data) {
    AddDocs({Doc(key, 1, std::move(data))});
  }

  void SetUpSingleValueFilter() {
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "count", model::Segment::kAscending));
    AddDoc("coll/val1", Map("count", 1));
    AddDoc("coll/val2", Map("count", 2));
    AddDoc("coll/val3", Map("count", 3));
  }

  void SetUpArrayValueFilter() {
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "values", model::Segment::kContains));
    AddDoc("coll/arr1", Map("values", Array(1, 2, 3)));
    AddDoc("coll/arr2", Map("values", Array(4, 5, 6)));
    AddDoc("coll/arr3", Map("values", Array(7, 8, 9)));
  }

  void SetUpMultipleOrderBys() {
    index_manager->AddFieldIndex(MakeFieldIndex(
        "coll", "a", model::Segment::kAscending, "b",
        model::Segment::kDescending, "c", model::Segment::kAscending));
    index_manager->AddFieldIndex(MakeFieldIndex(
        "coll", "a", model::Segment::kDescending, "b",
        model::Segment::kAscending, "c", model::Segment::kDescending));
    AddDoc("coll/val1", Map("a", 1, "b", 1, "c", 3));
    AddDoc("coll/val2", Map("a", 2, "b", 2, "c", 2));
    AddDoc("coll/val3", Map("a", 2, "b", 2, "c", 3));
    AddDoc("coll/val4", Map("a", 2, "b", 2, "c", 4));
    AddDoc("coll/val5", Map("a", 2, "b", 2, "c", 5));
    AddDoc("coll/val6", Map("a", 3, "b", 3, "c", 6));
  }

  void VerifyResults(const core::Query& query,
                     const std::vector<std::string>& documents) {
    auto target = query.ToTarget();
    absl::optional<std::vector<model::DocumentKey>> results =
        index_manager->GetDocumentsMatchingTarget(target);
    EXPECT_TRUE(results.has_value()) << "Target cannot be served from index.";
    std::vector<model::DocumentKey> expected;
    for (const auto& key : documents) {
      expected.push_back(Key(key));
    }
    EXPECT_EQ(expected, results.value())
        << "Query returned unexpected documents.";
  }

  std::unique_ptr<Persistence> persistence;
  IndexManager* index_manager;
};

TEST_F(LevelDbIndexManagerTest, AddsDocuments) {
  persistence->Run("AddsDocuments", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "exists", model::Segment::kAscending));
    AddDoc("coll/doc1", Map("exists", 1));
    AddDoc("coll/doc2", Map());
  });
}

TEST_F(LevelDbIndexManagerTest, OrderByFilter) {
  persistence->Run("TestOrderByFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "count", model::Segment::kAscending));
    AddDoc("coll/val1", Map("count", 1));
    AddDoc("coll/val2", Map("not-count", 2));
    AddDoc("coll/val3", Map("count", 3));
    auto query = Query("coll").AddingOrderBy(OrderBy("count"));
    VerifyResults(query, {"coll/val1", "coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, OrderByKeyFilter) {
  persistence->Run("TestOrderByKeyFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "count", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "count", model::Segment::kDescending));
    AddDoc("coll/val1", Map("count", 1));
    AddDoc("coll/val2", Map("count", 1));
    AddDoc("coll/val3", Map("count", 3));

    {
      SCOPED_TRACE("Verifing OrderByKey ASC");
      auto query = Query("coll").AddingOrderBy(OrderBy("count"));
      VerifyResults(query, {"coll/val1", "coll/val2", "coll/val3"});
    }

    {
      SCOPED_TRACE("Verifing OrderByKey DESC");
      auto query = Query("coll").AddingOrderBy(OrderBy("count", "desc"));
      VerifyResults(query, {"coll/val3", "coll/val2", "coll/val1"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, AscendingOrderWithLessThanFilter) {
  persistence->Run("TestAscendingOrderWithLessThanFilter", [&]() {
    index_manager->Start();
    SetUpMultipleOrderBys();

    auto original_query = Query("coll")
                              .AddingFilter(Filter("a", "==", 2))
                              .AddingFilter(Filter("b", "==", 2))
                              .AddingFilter(Filter("c", "<", 5))
                              .AddingOrderBy(OrderBy("c", "asc"));
    {
      SCOPED_TRACE("Verifing original");
      VerifyResults(original_query, {"coll/val2", "coll/val3", "coll/val4"});
    }
    {
      SCOPED_TRACE("Verifing non-restricted bound");
      auto query_with_non_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(1), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(6), /* inclusive= */ false));
      VerifyResults(query_with_non_restricted_bound,
                    {"coll/val2", "coll/val3", "coll/val4"});
    }
    {
      SCOPED_TRACE("Verifing restricted bound");
      auto query_with_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(2), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(4), /* inclusive= */ false));

      VerifyResults(query_with_restricted_bound, {"coll/val3"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, DescendingOrderWithLessThanFilter) {
  persistence->Run("TestDescendingOrderWithLessThanFilter", [&]() {
    index_manager->Start();
    SetUpMultipleOrderBys();

    auto original_query = Query("coll")
                              .AddingFilter(Filter("a", "==", 2))
                              .AddingFilter(Filter("b", "==", 2))
                              .AddingFilter(Filter("c", "<", 5))
                              .AddingOrderBy(OrderBy("c", "desc"));
    {
      SCOPED_TRACE("Verifying original");
      VerifyResults(original_query, {"coll/val4", "coll/val3", "coll/val2"});
    }
    {
      SCOPED_TRACE("Verifying non-restricted bound");
      auto query_with_non_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(6), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(1), /* inclusive= */ false));
      VerifyResults(query_with_non_restricted_bound,
                    {"coll/val4", "coll/val3", "coll/val2"});
    }
    {
      SCOPED_TRACE("Verifying restricted bound");
      auto query_with_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(4), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(2), /* inclusive= */ false));
      VerifyResults(query_with_restricted_bound, {"coll/val3"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, AscendingOrderWithGreaterThanFilter) {
  persistence->Run("TestAscendingOrderWithGreaterThanFilter", [&]() {
    index_manager->Start();
    SetUpMultipleOrderBys();

    auto original_query = Query("coll")
                              .AddingFilter(Filter("a", "==", 2))
                              .AddingFilter(Filter("b", "==", 2))
                              .AddingFilter(Filter("c", ">", 2))
                              .AddingOrderBy(OrderBy("c", "asc"));
    {
      SCOPED_TRACE("Verifying original");
      VerifyResults(original_query, {"coll/val3", "coll/val4", "coll/val5"});
    }
    {
      auto query_with_non_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(2), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(6), /* inclusive= */ false));
      SCOPED_TRACE("Verifying non-restricted bound");
      VerifyResults(query_with_non_restricted_bound,
                    {"coll/val3", "coll/val4", "coll/val5"});
    }
    {
      auto query_with_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(3), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(5), /* inclusive= */ false));
      SCOPED_TRACE("Verifying restricted bound");
      VerifyResults(query_with_restricted_bound, {"coll/val4"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, DescendingOrderWithGreaterThanFilter) {
  persistence->Run("TestDescendingOrderWithGreaterThanFilter", [&]() {
    index_manager->Start();
    SetUpMultipleOrderBys();

    auto original_query = Query("coll")
                              .AddingFilter(Filter("a", "==", 2))
                              .AddingFilter(Filter("b", "==", 2))
                              .AddingFilter(Filter("c", ">", 2))
                              .AddingOrderBy(OrderBy("c", "desc"));

    {
      SCOPED_TRACE("Verifying original");
      VerifyResults(original_query, {"coll/val5", "coll/val4", "coll/val3"});
    }
    {
      SCOPED_TRACE("Verifying non-restricted bound");
      auto query_with_non_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(6), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(2), /* inclusive= */ false));
      VerifyResults(query_with_non_restricted_bound,
                    {"coll/val5", "coll/val4", "coll/val3"});
    }
    {
      SCOPED_TRACE("Verifying restricted bound");
      auto query_with_restricted_bound =
          original_query
              .StartingAt(Bound::FromValue(Array(5), /* inclusive= */ false))
              .EndingAt(Bound::FromValue(Array(3), /* inclusive= */ false));
      VerifyResults(query_with_restricted_bound, {"coll/val4"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, CursorCannotExpandResult) {
  persistence->Run("TestDescendingOrderWithGreaterThanFilter", [&]() {
    index_manager->Start();

    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "c", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "c", model::Segment::kDescending));
    AddDoc("coll/val1", Map("a", 1, "b", 1, "c", 3));
    AddDoc("coll/val2", Map("a", 2, "b", 2, "c", 2));

    {
      auto query =
          Query("coll")
              .AddingFilter(Filter("c", ">", 2))
              .AddingOrderBy(OrderBy("c", "asc"))
              .StartingAt(Bound::FromValue(Array(2), /* inclusive */ true));
      VerifyResults(query, {"coll/val1"});
    }
    {
      auto query =
          Query("coll")
              .AddingFilter(Filter("c", "<", 3))
              .AddingOrderBy(OrderBy("c", "desc"))
              .StartingAt(Bound::FromValue(Array(3), /* inclusive */ true));
      VerifyResults(query, {"coll/val2"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, EqualityFilter) {
  persistence->Run("TestEqualityFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", "==", 2));
    VerifyResults(query, {"coll/val2"});
  });
}

TEST_F(LevelDbIndexManagerTest, OrderByWithNotEqualsFilter) {
  persistence->Run("TestOrderByWithNotEqualsFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "count", model::Segment::kAscending));
    AddDoc("coll/val1", Map("count", 1));
    AddDoc("coll/val2", Map("count", 2));

    auto query = Query("coll")
                     .AddingFilter(Filter("count", "!=", 2))
                     .AddingOrderBy(OrderBy("count"));
    VerifyResults(query, {"coll/val1"});
  });
}

TEST_F(LevelDbIndexManagerTest, NestedFieldEqualityFilter) {
  persistence->Run("TestNestedFieldEqualityFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "a.b", model::Segment::kAscending));
    AddDoc("coll/doc1", Map("a", Map("b", 1)));
    AddDoc("coll/doc2", Map("a", Map("b", 2)));
    auto query = Query("coll").AddingFilter(Filter("a.b", "==", 2));
    VerifyResults(query, {"coll/doc2"});
  });
}

TEST_F(LevelDbIndexManagerTest, NotEqualsFilter) {
  persistence->Run("TestNotEqualsFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", "!=", 2));
    VerifyResults(query, {"coll/val1", "coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, EqualsWithNotEqualsFilter) {
  persistence->Run("TestEqualsWithNotEqualsFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(MakeFieldIndex("coll", "a",
                                                model::Segment::kAscending, "b",
                                                model::Segment::kAscending));
    AddDoc("coll/val1", Map("a", 1, "b", 1));
    AddDoc("coll/val2", Map("a", 1, "b", 2));
    AddDoc("coll/val3", Map("a", 2, "b", 1));
    AddDoc("coll/val4", Map("a", 2, "b", 2));

    // Verifies that we apply the filter in the order of the field index
    {
      SCOPED_TRACE("Verifying equal then not-equal");
      auto query = Query("coll")
                       .AddingFilter(Filter("a", "==", 1))
                       .AddingFilter(Filter("b", "!=", 1));
      VerifyResults(query, {"coll/val2"});
    }

    {
      SCOPED_TRACE("Verifying not-equal then equal");
      auto query = Query("coll")
                       .AddingFilter(Filter("b", "!=", 1))
                       .AddingFilter(Filter("a", "==", 1));
      VerifyResults(query, {"coll/val2"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, EqualsWithNotEqualsFilterSameField) {
  persistence->Run("TestEqualsWithNotEqualsFilterSameField", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    {
      SCOPED_TRACE("Verifying > then !=");
      auto query = Query("coll")
                       .AddingFilter(Filter("count", ">", 1))
                       .AddingFilter(Filter("count", "!=", 2));
      VerifyResults(query, {"coll/val3"});
    }
    {
      SCOPED_TRACE("Verifying == then !=");
      auto query = Query("coll")
                       .AddingFilter(Filter("count", "==", 1))
                       .AddingFilter(Filter("count", "!=", 2));
      VerifyResults(query, {"coll/val1"});
    }
    {
      SCOPED_TRACE("Verifying == then != on same value");
      auto query = Query("coll")
                       .AddingFilter(Filter("count", "==", 1))
                       .AddingFilter(Filter("count", "!=", 1));
      VerifyResults(query, {});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, LessThanFilter) {
  persistence->Run("TestLessThanFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", "<", 2));
    VerifyResults(query, {"coll/val1"});
  });
}

TEST_F(LevelDbIndexManagerTest, LessThanOrEqualsFilter) {
  persistence->Run("TestLessThanOrEqualsFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", "<=", 2));
    VerifyResults(query, {"coll/val1", "coll/val2"});
  });
}

TEST_F(LevelDbIndexManagerTest, GreaterThanOrEqualsFilter) {
  persistence->Run("TestGreaterThanOrEqualsFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", ">=", 2));
    VerifyResults(query, {"coll/val2", "coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, GreaterThanFilter) {
  persistence->Run("TestGreaterThanFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", ">", 2));
    VerifyResults(query, {"coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, RangeFilter) {
  persistence->Run("TestRangeFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll")
                     .AddingFilter(Filter("count", ">", 1))
                     .AddingFilter(Filter("count", "<", 3));
    VerifyResults(query, {"coll/val2"});
  });
}

TEST_F(LevelDbIndexManagerTest, StartAtFilter) {
  persistence->Run("TestStartAtFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query =
        Query("coll")
            .AddingOrderBy(OrderBy("count"))
            .StartingAt(Bound::FromValue(Array(2), /* inclusive= */ true));
    VerifyResults(query, {"coll/val2", "coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, AppliesStartAtFilterWithNotIn) {
  persistence->Run("TestAppliesStartAtFilterWithNotIn", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query =
        Query("coll")
            .AddingFilter(Filter("count", "!=", 2))
            .AddingOrderBy(OrderBy("count"))
            .StartingAt(Bound::FromValue(Array(2), /* inclusive= */ true));
    VerifyResults(query, {"coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, StartAfterFilter) {
  persistence->Run("TestStartAfterFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query =
        Query("coll")
            .AddingOrderBy(OrderBy("count"))
            .StartingAt(Bound::FromValue(Array(2), /* inclusive= */ false));
    VerifyResults(query, {"coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, EndAtFilter) {
  persistence->Run("TestEndAtFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query =
        Query("coll")
            .AddingOrderBy(OrderBy("count"))
            .EndingAt(Bound::FromValue(Array(2), /* inclusive= */ true));
    VerifyResults(query, {"coll/val1", "coll/val2"});
  });
}

TEST_F(LevelDbIndexManagerTest, EndBeforeFilter) {
  persistence->Run("TestEndBeforeFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query =
        Query("coll")
            .AddingOrderBy(OrderBy("count"))
            .EndingAt(Bound::FromValue(Array(2), /* inclusive= */ false));
    VerifyResults(query, {"coll/val1"});
  });
}

TEST_F(LevelDbIndexManagerTest, RangeWithBoundFilter) {
  persistence->Run("TestRangeWithBoundFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto start_at =
        Query("coll")
            .AddingFilter(Filter("count", ">=", 1))
            .AddingFilter(Filter("count", "<=", 3))
            .AddingOrderBy(OrderBy("count"))
            .StartingAt(Bound::FromValue(Array(1), /* inclusive= */ false))
            .EndingAt(Bound::FromValue(Array(2), /* inclusive= */ true));
    VerifyResults(start_at, {"coll/val2"});
  });
}

TEST_F(LevelDbIndexManagerTest, InFilter) {
  persistence->Run("TestInFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", "in", Array(1, 3)));
    VerifyResults(query, {"coll/val1", "coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, NotInFilter) {
  persistence->Run("TestNotInFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query =
        Query("coll").AddingFilter(Filter("count", "not-in", Array(1, 2)));
    VerifyResults(query, {"coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, NotInWithGreaterThanFilter) {
  persistence->Run("TestNotInWithGreaterThanFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll")
                     .AddingFilter(Filter("count", ">", 1))
                     .AddingFilter(Filter("count", "not-in", Array(2)));
    VerifyResults(query, {"coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, OutOfBoundsNotInWithGreaterThanFilter) {
  persistence->Run("TestOutOfBoundsNotInWithGreaterThanFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll")
                     .AddingFilter(Filter("count", ">", 2))
                     .AddingFilter(Filter("count", "not-in", Array(1)));
    VerifyResults(query, {"coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, ArrayContainsFilter) {
  persistence->Run("TestArrayContainsFilter", [&]() {
    index_manager->Start();
    SetUpArrayValueFilter();
    auto query =
        Query("coll").AddingFilter(Filter("values", "array-contains", 1));
    VerifyResults(query, {"coll/arr1"});
  });
}

TEST_F(LevelDbIndexManagerTest, ArrayContainsWithNotEqualsFilter) {
  persistence->Run("TestArrayContainsWithNotEqualsFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(MakeFieldIndex("coll", "a",
                                                model::Segment::kContains, "b",
                                                model::Segment::kAscending));
    AddDoc("coll/val1", Map("a", Array(1), "b", 1));
    AddDoc("coll/val2", Map("a", Array(1), "b", 2));
    AddDoc("coll/val3", Map("a", Array(2), "b", 1));
    AddDoc("coll/val4", Map("a", Array(2), "b", 2));

    auto query = Query("coll")
                     .AddingFilter(Filter("a", "array-contains", 1))
                     .AddingFilter(Filter("b", "!=", 1));
    VerifyResults(query, {"coll/val2"});
  });
}

TEST_F(LevelDbIndexManagerTest,
       TestArrayContainsWithNotEqualsFilterOnSameField) {
  persistence->Run("TestArrayContainsWithNotEqualsFilterOnSameField", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(MakeFieldIndex("coll", "a",
                                                model::Segment::kContains, "a",
                                                model::Segment::kAscending));
    AddDoc("coll/val1", Map("a", Array(1, 1)));
    AddDoc("coll/val2", Map("a", Array(1, 2)));
    AddDoc("coll/val3", Map("a", Array(2, 1)));
    AddDoc("coll/val4", Map("a", Array(2, 2)));

    auto query = Query("coll")
                     .AddingFilter(Filter("a", "array-contains", 1))
                     .AddingFilter(Filter("a", "!=", Array(1, 2)));
    VerifyResults(query, {"coll/val1", "coll/val3"});
  });
}

TEST_F(LevelDbIndexManagerTest, EqualsWithNotEqualsOnSameField) {
  persistence->Run("TestEqualsWithNotEqualsOnSameField", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();

    std::vector<
        std::pair<std::vector<core::FieldFilter>, std::vector<std::string>>>
        filtersAndResults = {
            {{Filter("count", ">", 1), Filter("count", "!=", 2)},
             {"coll/val3"}},
            {{Filter("count", "==", 1), Filter("count", "!=", 2)},
             {"coll/val1"}},
            {{Filter("count", "==", 1), Filter("count", "!=", 1)}, {}},
            {{Filter("count", ">", 2), Filter("count", "!=", 2)},
             {"coll/val3"}},
            {{Filter("count", ">=", 2), Filter("count", "!=", 2)},
             {"coll/val3"}},
            {{Filter("count", "<=", 2), Filter("count", "!=", 2)},
             {"coll/val1"}},
            {{Filter("count", "<=", 2), Filter("count", "!=", 1)},
             {"coll/val2"}},
            {{Filter("count", "<", 2), Filter("count", "!=", 2)},
             {"coll/val1"}},
            {{Filter("count", "<", 2), Filter("count", "!=", 1)}, {}},
            {{Filter("count", ">", 2), Filter("count", "not-in", Array(3))},
             {}},
            {{Filter("count", ">=", 2), Filter("count", "not-in", Array(3))},
             {"coll/val2"}},
            {{Filter("count", ">=", 2), Filter("count", "not-in", Array(3, 3))},
             {"coll/val2"}},
            {{Filter("count", ">", 1), Filter("count", "<", 3),
              Filter("count", "!=", 2)},
             {}},
            {{Filter("count", ">=", 1), Filter("count", "<", 3),
              Filter("count", "!=", 2)},
             {"coll/val1"}},
            {{Filter("count", ">=", 1), Filter("count", "<=", 3),
              Filter("count", "!=", 2)},
             {"coll/val1", "coll/val3"}},
            {{Filter("count", ">", 1), Filter("count", "<=", 3),
              Filter("count", "!=", 2)},
             {"coll/val3"}}};

    size_t counter = 0;
    for (const auto& filter_result_pair : filtersAndResults) {
      auto query = Query("coll");
      for (const auto& filter : filter_result_pair.first) {
        query = query.AddingFilter(filter);
      }
      SCOPED_TRACE(absl::StrCat("Verifing case#", counter++));
      VerifyResults(query, filter_result_pair.second);
    }
  });
}

TEST_F(LevelDbIndexManagerTest, ArrayContainsAnyFilter) {
  persistence->Run("TestArrayContainsAnyFilter", [&]() {
    index_manager->Start();
    SetUpArrayValueFilter();
    auto query = Query("coll").AddingFilter(
        Filter("values", "array-contains-any", Array(1, 2, 4)));
    VerifyResults(query, {"coll/arr1", "coll/arr2"});
  });
}

TEST_F(LevelDbIndexManagerTest, ArrayContainsDoesNotMatchNonArray) {
  persistence->Run("TestArrayContainsDoesNotMatchNonArray", [&]() {
    index_manager->Start();
    // Set up two field indices. This causes two index entries to be written,
    // but our query should only use one index.
    SetUpArrayValueFilter();
    SetUpSingleValueFilter();
    AddDoc("coll/nonmatching", Map("values", 1));
    auto query = Query("coll").AddingFilter(
        Filter("values", "array-contains-any", Array(1)));
    VerifyResults(query, {"coll/arr1"});
  });
}

TEST_F(LevelDbIndexManagerTest, NoMatchingFilter) {
  persistence->Run("TestNoMatchingFilter", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("unknown", "==", true));
    EXPECT_FALSE(index_manager->GetFieldIndex(query.ToTarget()).has_value());
    EXPECT_FALSE(index_manager->GetDocumentsMatchingTarget(query.ToTarget())
                     .has_value());
  });
}

TEST_F(LevelDbIndexManagerTest, NoMatchingDocs) {
  persistence->Run("TestNoMatchingDocs", [&]() {
    index_manager->Start();
    SetUpSingleValueFilter();
    auto query = Query("coll").AddingFilter(Filter("count", "==", -1));
    VerifyResults(query, {});
  });
}

TEST_F(LevelDbIndexManagerTest, EqualityFilterWithNonMatchingType) {
  persistence->Run("TestEqualityFilterWithNonMatchingType", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "value", model::Segment::kAscending));
    AddDoc("coll/boolean", Map("value", true));
    AddDoc("coll/string", Map("value", "true"));
    AddDoc("coll/number", Map("value", 1));
    auto query = Query("coll").AddingFilter(Filter("value", "==", true));
    VerifyResults(query, {"coll/boolean"});
  });
}

TEST_F(LevelDbIndexManagerTest, CollectionGroup) {
  persistence->Run("TestCollectionGroup", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll1", "value", model::Segment::kAscending));
    AddDoc("coll1/doc1", Map("value", true));
    AddDoc("coll2/doc2/coll1/doc1", Map("value", true));
    AddDoc("coll2/doc2", Map("value", true));
    auto query =
        CollectionGroupQuery("coll1").AddingFilter(Filter("value", "==", true));
    VerifyResults(query, {"coll1/doc1", "coll2/doc2/coll1/doc1"});
  });
}

TEST_F(LevelDbIndexManagerTest, LimitFilter) {
  persistence->Run("TestLimitFilter", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "value", model::Segment::kAscending));
    AddDoc("coll/doc1", Map("value", 1));
    AddDoc("coll/doc2", Map("value", 1));
    AddDoc("coll/doc3", Map("value", 1));
    auto query = Query("coll")
                     .AddingFilter(Filter("value", "==", 1))
                     .WithLimitToFirst(2);
    VerifyResults(query, {"coll/doc1", "coll/doc2"});
  });
}

TEST_F(LevelDbIndexManagerTest, LimitAppliesOrdering) {
  persistence->Run("TestLimitAppliesOrdering", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "value", model::Segment::kContains, "value",
                       model::Segment::kAscending));
    AddDoc("coll/doc1", Map("value", Array(1, "foo")));
    AddDoc("coll/doc2", Map("value", Array(3, "foo")));
    AddDoc("coll/doc3", Map("value", Array(2, "foo")));
    auto query = Query("coll")
                     .AddingFilter(Filter("value", "array-contains", "foo"))
                     .AddingOrderBy(OrderBy("value"))
                     .WithLimitToFirst(2);
    VerifyResults(query, {"coll/doc1", "coll/doc3"});
  });
}

TEST_F(LevelDbIndexManagerTest, IndexEntriesAreUpdated) {
  persistence->Run("TestIndexEntriesAreUpdated", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "value", model::Segment::kAscending));
    auto query = Query("coll").AddingOrderBy(OrderBy("value"));

    AddDoc("coll/doc1", Map("value", true));
    {
      SCOPED_TRACE("With doc1");
      VerifyResults(query, {"coll/doc1"});
    }

    AddDocs(
        {Doc("coll/doc1", 1, Map()), Doc("coll/doc2", 1, Map("value", true))});
    {
      SCOPED_TRACE("With doc1 (non-matching) and doc2");
      VerifyResults(query, {"coll/doc2"});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, IndexEntriesAreUpdatedWithDeletedDoc) {
  persistence->Run("TestIndexEntriesAreUpdatedWithDeletedDoc", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "value", model::Segment::kAscending));
    auto query = Query("coll").AddingOrderBy(OrderBy("value"));

    AddDoc("coll/doc1", Map("value", true));
    {
      SCOPED_TRACE("With doc1");
      VerifyResults(query, {"coll/doc1"});
    }

    AddDocs({DeletedDoc("coll/doc1", 1)});
    {
      SCOPED_TRACE("With deleted doc1");
      VerifyResults(query, {});
    }
  });
}

TEST_F(LevelDbIndexManagerTest, AdvancedQueries) {
  // This test compares local query results with those received from the Java
  // Server SDK.
  persistence->Run("TestAdvancedQueries", [&]() {
    index_manager->Start();
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "null", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "int", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "float", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "string", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "multi", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "array", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "array", model::Segment::kDescending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "array", model::Segment::kContains));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "map", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "map.field", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "prefix", model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "prefix", model::Segment::kAscending, "suffix",
                       model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "a", model::Segment::kAscending));
    index_manager->AddFieldIndex(MakeFieldIndex("coll", "a",
                                                model::Segment::kAscending, "b",
                                                model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "a", model::Segment::kDescending, "b",
                       model::Segment::kAscending));
    index_manager->AddFieldIndex(MakeFieldIndex("coll", "a",
                                                model::Segment::kAscending, "b",
                                                model::Segment::kDescending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll", "a", model::Segment::kDescending, "b",
                       model::Segment::kDescending));
    index_manager->AddFieldIndex(MakeFieldIndex("coll", "b",
                                                model::Segment::kAscending, "a",
                                                model::Segment::kAscending));

    std::vector<nanopb::Message<google_firestore_v1_Value>> data;
    data.push_back(Map());
    data.push_back(Map("array", Array(1, "foo"), "int", 1));
    data.push_back(Map("array", Array(2, "foo")));
    data.push_back(Map("array", Array(3, "foo"), "int", 3));
    data.push_back(Map("array", "foo"));
    data.push_back(Map("array", Array(1)));
    data.push_back(Map("float", -0.0, "string", "a"));
    data.push_back(Map("float", 0, "string", "ab"));
    data.push_back(Map("float", 0.0, "string", "b"));
    data.push_back(Map("float", std::numeric_limits<double>::quiet_NaN()));
    data.push_back(Map("multi", true));
    data.push_back(Map("multi", 1));
    data.push_back(Map("multi", "string"));
    data.push_back(Map("multi", Array()));
    data.push_back(Map("null", nullptr));
    data.push_back(Map("prefix", Array(1, 2), "suffix", nullptr));
    data.push_back(Map("prefix", Array(1), "suffix", 2));
    data.push_back(Map("map", Map()));
    data.push_back(Map("map", Map("field", true)));
    data.push_back(Map("map", Map("field", false)));
    data.push_back(Map("a", 0, "b", 0));
    data.push_back(Map("a", 0, "b", 1));
    data.push_back(Map("a", 1, "b", 0));
    data.push_back(Map("a", 1, "b", 1));
    data.push_back(Map("a", 2, "b", 0));
    data.push_back(Map("a", 2, "b", 1));

    for (auto& map : data) {
      for (size_t idx = 1; idx < map->map_value.fields_count; ++idx) {
        ASSERT_LE(nanopb::MakeStringView(map->map_value.fields[idx - 1].key),
                  nanopb::MakeStringView(map->map_value.fields[idx].key))
            << "Expect fields in testing documents to be sorted by key.";
      }

      auto doc_id = "coll/" + model::CanonicalId(*map);
      AddDoc(doc_id, std::move(map));
    }

    auto q = Query("coll");

    std::vector<std::pair<core::Query, std::vector<std::string>>> test_cases = {
        {q.AddingOrderBy(OrderBy("int")),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[3,foo],int:3}"}},
        {q.AddingFilter(
             Filter("float", "==", std::numeric_limits<double>::quiet_NaN())),
         {"coll/{float:nan}"}},
        {q.AddingFilter(Filter("float", "==", -0.0)),
         {"coll/{float:-0.0,string:a}", "coll/{float:0,string:ab}",
          "coll/{float:0.0,string:b}"}},
        {q.AddingFilter(Filter("float", "==", 0)),
         {"coll/{float:-0.0,string:a}", "coll/{float:0,string:ab}",
          "coll/{float:0.0,string:b}"}},
        {q.AddingFilter(Filter("float", "==", 0.0)),
         {"coll/{float:-0.0,string:a}", "coll/{float:0,string:ab}",
          "coll/{float:0.0,string:b}"}},
        {q.AddingFilter(Filter("string", "==", "a")),
         {"coll/{float:-0.0,string:a}"}},
        {q.AddingFilter(Filter("string", ">", "a")),
         {"coll/{float:0,string:ab}", "coll/{float:0.0,string:b}"}},
        {q.AddingFilter(Filter("string", ">=", "a")),
         {"coll/{float:-0.0,string:a}", "coll/{float:0,string:ab}",
          "coll/{float:0.0,string:b}"}},
        {q.AddingFilter(Filter("string", "<", "b")),
         {"coll/{float:-0.0,string:a}", "coll/{float:0,string:ab}"}},
        {q.AddingFilter(Filter("string", "<", "coll")),
         {"coll/{float:-0.0,string:a}", "coll/{float:0,string:ab}",
          "coll/{float:0.0,string:b}"}},
        {q.AddingFilter(Filter("string", ">", "a"))
             .AddingFilter(Filter("string", "<", "b")),
         {"coll/{float:0,string:ab}"}},
        {q.AddingFilter(Filter("array", "array-contains", "foo")),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[2,foo]}",
          "coll/{array:[3,foo],int:3}"}},
        {q.AddingFilter(Filter("array", "array-contains-any", Array(1, "foo"))),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}",
          "coll/{array:[2,foo]}", "coll/{array:[3,foo],int:3}"}},
        {q.AddingFilter(Filter("multi", ">=", true)), {"coll/{multi:true}"}},
        {q.AddingFilter(Filter("multi", ">=", 0)), {"coll/{multi:1}"}},
        {q.AddingFilter(Filter("multi", ">=", "")), {"coll/{multi:string}"}},
        {q.AddingFilter(Filter("multi", ">=", Array())), {"coll/{multi:[]}"}},
        {q.AddingFilter(Filter("multi", "!=", true)),
         {"coll/{multi:1}", "coll/{multi:string}", "coll/{multi:[]}"}},
        {q.AddingFilter(Filter("multi", "in", Array(true, 1))),
         {"coll/{multi:true}", "coll/{multi:1}"}},
        {q.AddingFilter(Filter("multi", "not-in", Array(true, 1))),
         {"coll/{multi:string}", "coll/{multi:[]}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .StartingAt(Bound::FromValue(Array(Array(2)), true)),
         {"coll/{array:[2,foo]}", "coll/{array:[3,foo],int:3}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .StartingAt(Bound::FromValue(Array(Array(2)), true)),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}",
          "coll/{array:foo}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .StartingAt(Bound::FromValue(Array(Array(2)), true))
             .WithLimitToFirst(2),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .StartingAt(Bound::FromValue(Array(Array(2)), false)),
         {"coll/{array:[2,foo]}", "coll/{array:[3,foo],int:3}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .StartingAt(Bound::FromValue(Array(Array(2)), false)),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}",
          "coll/{array:foo}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .StartingAt(Bound::FromValue(Array(Array(2)), false))
             .WithLimitToFirst(2),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .StartingAt(Bound::FromValue(Array(Array(2, "foo")), false)),
         {"coll/{array:[3,foo],int:3}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .StartingAt(Bound::FromValue(Array(Array(2, "foo")), false)),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}",
          "coll/{array:foo}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .StartingAt(Bound::FromValue(Array(Array(2, "foo")), false))
             .WithLimitToFirst(2),
         {"coll/{array:[1,foo],int:1}", "coll/{array:[1]}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .EndingAt(Bound::FromValue(Array(Array(2)), true)),
         {"coll/{array:foo}", "coll/{array:[1]}",
          "coll/{array:[1,foo],int:1}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .EndingAt(Bound::FromValue(Array(Array(2)), true)),
         {"coll/{array:[3,foo],int:3}", "coll/{array:[2,foo]}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .EndingAt(Bound::FromValue(Array(Array(2)), false)),
         {"coll/{array:foo}", "coll/{array:[1]}",
          "coll/{array:[1,foo],int:1}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .EndingAt(Bound::FromValue(Array(Array(2)), false))
             .WithLimitToFirst(2),
         {"coll/{array:foo}", "coll/{array:[1]}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .EndingAt(Bound::FromValue(Array(Array(2)), false)),
         {"coll/{array:[3,foo],int:3}", "coll/{array:[2,foo]}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .EndingAt(Bound::FromValue(Array(Array(2, "foo")), false)),
         {"coll/{array:foo}", "coll/{array:[1]}",
          "coll/{array:[1,foo],int:1}"}},
        {q.AddingOrderBy(OrderBy("array"))
             .EndingAt(Bound::FromValue(Array(Array(2, "foo")), false))
             .WithLimitToFirst(2),
         {"coll/{array:foo}", "coll/{array:[1]}"}},
        {q.AddingOrderBy(OrderBy("array", "desc"))
             .EndingAt(Bound::FromValue(Array(Array(2, "foo")), false)),
         {"coll/{array:[3,foo],int:3}"}},
        {q.AddingOrderBy(OrderBy("a"))
             .AddingOrderBy(OrderBy("b"))
             .WithLimitToFirst(1),
         {"coll/{a:0,b:0}"}},
        {q.AddingOrderBy(OrderBy("a", "desc"))
             .AddingOrderBy(OrderBy("b"))
             .WithLimitToFirst(1),
         {"coll/{a:2,b:0}"}},
        {q.AddingOrderBy(OrderBy("a"))
             .AddingOrderBy(OrderBy("b", "desc"))
             .WithLimitToFirst(1),
         {"coll/{a:0,b:1}"}},
        {q.AddingOrderBy(OrderBy("a", "desc"))
             .AddingOrderBy(OrderBy("b", "desc"))
             .WithLimitToFirst(1),
         {"coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("a", ">", 0)).AddingFilter(Filter("b", "==", 1)),
         {"coll/{a:1,b:1}", "coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("a", "==", 1))
             .AddingFilter(Filter("b", "==", 1)),
         {"coll/{a:1,b:1}"}},
        {q.AddingFilter(Filter("a", "!=", 0))
             .AddingFilter(Filter("b", "==", 1)),
         {"coll/{a:1,b:1}", "coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("b", "==", 1))
             .AddingFilter(Filter("a", "!=", 0)),
         {"coll/{a:1,b:1}", "coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("a", "not-in", Array(0, 1))),
         {"coll/{a:2,b:0}", "coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("a", "not-in", Array(0, 1)))
             .AddingFilter(Filter("b", "==", 1)),
         {"coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("b", "==", 1))
             .AddingFilter(Filter("a", "not-in", Array(0, 1))),
         {"coll/{a:2,b:1}"}},
        {q.AddingFilter(Filter("null", "==", nullptr)), {"coll/{null:null}"}},
        {q.AddingOrderBy(OrderBy("null")), {"coll/{null:null}"}},
        {q.AddingFilter(Filter("prefix", "==", Array(1, 2))),
         {"coll/{prefix:[1,2],suffix:null}"}},
        {q.AddingFilter(Filter("prefix", "==", Array(1)))
             .AddingFilter(Filter("suffix", "==", 2)),
         {"coll/{prefix:[1],suffix:2}"}},
        {q.AddingFilter(Filter("map", "==", Map())), {"coll/{map:{}}"}},
        {q.AddingFilter(Filter("map", "==", Map("field", true))),
         {"coll/{map:{field:true}}"}},
        {q.AddingFilter(Filter("map.field", "==", true)),
         {"coll/{map:{field:true}}"}},
        {q.AddingOrderBy(OrderBy("map")),
         {"coll/{map:{}}", "coll/{map:{field:false}}",
          "coll/{map:{field:true}}"}},
        {q.AddingOrderBy(OrderBy("map.field")),
         {"coll/{map:{field:false}}", "coll/{map:{field:true}}"}}};

    size_t counter = 0;
    for (const auto& test : test_cases) {
      SCOPED_TRACE(
          absl::StrCat("Test case#", counter, ": ", test.first.CanonicalId()));
      VerifyResults(test.first, test.second);
    }
  });
}

TEST_F(LevelDbIndexManagerTest, CreateReadFieldsIndexes) {
  persistence->Run("CreateReadDeleteFieldsIndexes", [&]() {
    IndexManager* index_manager =
        persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    index_manager->AddFieldIndex(
        MakeFieldIndex("coll1", 1, model::FieldIndex::InitialState(), "value",
                       model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll2", 2, model::FieldIndex::InitialState(), "value",
                       model::Segment::kContains));

    {
      auto indexes = index_manager->GetFieldIndexes("coll1");
      EXPECT_EQ(indexes.size(), 1);
      // Note index_id() is 0 because index manager rewrites it using its
      // internal id.
      EXPECT_EQ(indexes[0].index_id(), 0);
      EXPECT_EQ(indexes[0].collection_group(), "coll1");
    }

    index_manager->AddFieldIndex(
        MakeFieldIndex("coll1", 3, model::FieldIndex::InitialState(),
                       "newValue", model::Segment::kContains));
    {
      auto indexes = index_manager->GetFieldIndexes("coll1");
      EXPECT_EQ(indexes.size(), 2);
      EXPECT_EQ(indexes[0].collection_group(), "coll1");
      EXPECT_EQ(indexes[1].collection_group(), "coll1");
    }

    {
      auto indexes = index_manager->GetFieldIndexes("coll2");
      EXPECT_EQ(indexes.size(), 1);
      EXPECT_EQ(indexes[0].collection_group(), "coll2");
    }
  });
}

TEST_F(LevelDbIndexManagerTest,
       NextCollectionGroupAdvancesWhenCollectionIsUpdated) {
  persistence->Run("CreateReadDeleteFieldsIndexes", [&]() {
    IndexManager* index_manager =
        persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    index_manager->AddFieldIndex(MakeFieldIndex("coll1"));
    index_manager->AddFieldIndex(MakeFieldIndex("coll2"));

    {
      const auto& collection_group =
          index_manager->GetNextCollectionGroupToUpdate();
      EXPECT_TRUE(collection_group.has_value());
      EXPECT_EQ(collection_group.value(), "coll1");
    }

    index_manager->UpdateCollectionGroup("coll1", IndexOffset::None());
    {
      const auto& collection_group =
          index_manager->GetNextCollectionGroupToUpdate();
      EXPECT_TRUE(collection_group.has_value());
      EXPECT_EQ(collection_group.value(), "coll2");
    }

    index_manager->UpdateCollectionGroup("coll2", IndexOffset::None());
    {
      const auto& collection_group =
          index_manager->GetNextCollectionGroupToUpdate();
      EXPECT_TRUE(collection_group.has_value());
      EXPECT_EQ(collection_group.value(), "coll1");
    }
  });
}

TEST_F(LevelDbIndexManagerTest, PersistsIndexOffset) {
  persistence->Run("CreateReadDeleteFieldsIndexes", [&]() {
    IndexManager* index_manager =
        persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    index_manager->AddFieldIndex(
        MakeFieldIndex("coll1", "value", model::Segment::kAscending));
    IndexOffset offset{Version(20), Key("coll/doc"), 42};
    index_manager->UpdateCollectionGroup("coll1", offset);

    index_manager =
        persistence->GetIndexManager(credentials::User::Unauthenticated());
    index_manager->Start();

    std::vector<FieldIndex> indexes = index_manager->GetFieldIndexes("coll1");
    EXPECT_EQ(indexes.size(), 1);
    FieldIndex index = indexes[0];
    EXPECT_EQ(index.index_state().index_offset(), offset);
  });
}

TEST_F(LevelDbIndexManagerTest, DeleteFieldsIndexeRemovesAllMetadata) {
  persistence->Run("CreateReadDeleteFieldsIndexes", [&]() {
    IndexManager* index_manager =
        persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    auto index = MakeFieldIndex("coll1", 0, model::FieldIndex::InitialState(),
                                "value", model::Segment::kAscending);
    index_manager->AddFieldIndex(index);
    {
      auto indexes = index_manager->GetFieldIndexes("coll1");
      EXPECT_EQ(indexes.size(), 1);
    }

    index_manager->DeleteFieldIndex(index);
    {
      auto indexes = index_manager->GetFieldIndexes("coll1");
      EXPECT_EQ(indexes.size(), 0);
    }
  });
}

TEST_F(LevelDbIndexManagerTest,
       DeleteFieldIndexRemovesEntryFromCollectionGroup) {
  persistence->Run("CreateReadDeleteFieldsIndexes", [&]() {
    IndexManager* index_manager =
        persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    index_manager->AddFieldIndex(
        MakeFieldIndex("coll1", 1, IndexState{1, IndexOffset::None()}, "value",
                       model::Segment::kAscending));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll2", 2, IndexState{2, IndexOffset::None()}, "value",
                       model::Segment::kContains));
    auto collection_group = index_manager->GetNextCollectionGroupToUpdate();
    EXPECT_TRUE(collection_group);
    EXPECT_EQ(collection_group.value(), "coll1");

    std::vector<FieldIndex> indexes = index_manager->GetFieldIndexes("coll1");
    EXPECT_EQ(indexes.size(), 1);
    index_manager->DeleteFieldIndex(indexes[0]);
    collection_group = index_manager->GetNextCollectionGroupToUpdate();
    EXPECT_EQ(collection_group, "coll2");
  });
}

TEST_F(LevelDbIndexManagerTest, CanChangeUser) {
  persistence->Run("CreateReadDeleteFieldsIndexes", [&]() {
    IndexManager* index_manager =
        persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    // Add two indexes and mark one as updated.
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll1", 1, FieldIndex::InitialState()));
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll2", 2, FieldIndex::InitialState()));
    index_manager->UpdateCollectionGroup("coll2", IndexOffset::None());

    VerifySequenceNumber(index_manager, "coll1", 0);
    VerifySequenceNumber(index_manager, "coll2", 1);

    // New user signs it. The user should see all existing field indices.
    // Sequence numbers are set to 0.
    index_manager = persistence->GetIndexManager(User("authenticated"));
    index_manager->Start();

    // Add a new index and mark it as updated.
    index_manager->AddFieldIndex(
        MakeFieldIndex("coll3", 2, FieldIndex::InitialState()));
    index_manager->UpdateCollectionGroup("coll3", IndexOffset::None());

    VerifySequenceNumber(index_manager, "coll1", 0);
    VerifySequenceNumber(index_manager, "coll2", 0);
    VerifySequenceNumber(index_manager, "coll3", 1);

    // Original user signs it. The user should also see the new index with a
    // zero sequence number.
    index_manager = persistence->GetIndexManager(User::Unauthenticated());
    index_manager->Start();

    VerifySequenceNumber(index_manager, "coll1", 0);
    VerifySequenceNumber(index_manager, "coll2", 1);
    VerifySequenceNumber(index_manager, "coll3", 0);
  });
}

}  // namespace local
}  // namespace firestore
}  // namespace firebase
