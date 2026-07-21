#
# Copyright (c) "Neo4j"
# Neo4j Sweden AB [https://neo4j.com]
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Attribution Notice under the terms of the Apache License 2.0
#
# This work was created by the collective efforts of the openCypher community.
# Without limiting the terms of Section 6, any Derivative Work that is not
# approved by the public consensus process of the openCypher Implementers Group
# should not be described as “Cypher” (and Cypher® is a registered trademark of
# Neo4j Inc.) or as "openCypher". Extensions by implementers or prototypes or
# proposals for change that have been documented or implemented should only be
# described as "implementation extensions to Cypher" or as "proposed changes to
# Cypher that are not yet approved by the openCypher community".
#

#encoding: utf-8
#
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/with/With4.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: With4 - Variable aliasing
  # correctly aliasing variables

  Scenario: [1] Aliasing relationship variable
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:T1]->(),
             ()-[:T2]->()
      """
    When executing query:
      """
      MATCH ()-[r1]->()
      LET __gql_with_scope_1 = 1, r2 = r1
      RETURN r2 AS rel
      """
    Then the result should be, in any order:
      | rel   |
      | [:T1] |
      | [:T2] |
    And no side effects

  Scenario: [2] Aliasing expression to new variable name
    Given an empty graph
    And having executed:
      """
      INSERT (:Begin {num: 42}),
             (:End {num: 42}),
             (:End {num: 3})
      """
    When executing query:
      """
      MATCH (a:Begin)
      LET __gql_with_scope_1 = 1, property = a.num
      MATCH (b:End)
      WHERE property = b.num
      RETURN b
      """
    Then the result should be, in any order:
      | b                |
      | (:End {num: 42}) |
    And no side effects

  Scenario: [3] Aliasing expression to existing variable name
    Given an empty graph
    And having executed:
      """
      INSERT ({num: 1, name: 'King Kong'}),
        ({num: 2, name: 'Ann Darrow'})
      """
    When executing query:
      """
      MATCH (n)
      LET __gql_with_scope_1 = 1, n = n.name
      RETURN n
      """
    Then the result should be, in any order:
      | n            |
      | 'Ann Darrow' |
      | 'King Kong'  |
    And no side effects

  Scenario: [4] Fail when forwarding multiple aliases with the same name
    Given any graph
    When executing query:
      """
      LET __gql_with_scope_1 = 1, a = 1, a = 2
      RETURN a
      """
    Then a SyntaxError should be raised at compile time: ColumnNameConflict

  Scenario: [5] Fail when not aliasing expressions in WITH
    Given any graph
    When executing query:
      """
      MATCH (a)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, __gql_with_1_2_count = count(*)
      RETURN a
      """
    Then a SyntaxError should be raised at compile time: NoExpressionAlias

  Scenario: [6] Reusing variable names in WITH
    Given an empty graph
    And having executed:
      """
      INSERT (a:Person), (b:Person), (m:Message {id: 10})
      INSERT (a)-[:LIKE {creationDate: 20160614}]->(m)-[:POSTED_BY]->(b)
      """
    When executing query:
      """
      MATCH (person:Person)<--(message)<-[like]-(:Person)
      LET __gql_with_scope_1 = 1, likeTime = like.creationDate, __gql_with_1_2_person_AS_person = person AS person
        ORDER BY likeTime, message.id
      LET __gql_with_scope_2 = 1, latestLike = head(collect({likeTime: likeTime})), __gql_with_2_2_person_AS_person = person AS person
      LET __gql_with_scope_3 = 1, likeTime = latestLike.likeTime
        ORDER BY likeTime
      RETURN likeTime
      """
    Then the result should be, in order:
      | likeTime |
      | 20160614 |
    And no side effects

  Scenario: [7] Multiple aliasing and backreferencing
    Given any graph
    When executing query:
      """
      INSERT (m {id: 0})
      LET __gql_with_scope_1 = 1, m = {first: m.id}
      LET __gql_with_scope_2 = 1, m = {second: m.first}
      RETURN m.second
      """
    Then the result should be, in any order:
      | m.second |
      | 0        |
    And the side effects should be:
      | +nodes      | 1 |
      | +properties | 1 |
