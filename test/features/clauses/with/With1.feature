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
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/with/With1.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: With1 - Forward single variable
  # correctly forward of values according to their type, no other effects

  Scenario: [1] Forwarind a node variable 1
    Given an empty graph
    And having executed:
      """
      INSERT (:A)-[:REL]->(:B)
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a
      MATCH (a)-->(b)
      RETURN *
      """
    Then the result should be, in any order:
      | a    | b    |
      | (:A) | (:B) |
    And no side effects

  Scenario: [2] Forwarind a node variable 2
    Given an empty graph
    And having executed:
      """
      INSERT (:A)-[:REL]->(:B)
      INSERT (:X)
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a
      MATCH (x:X), (a)-->(b)
      RETURN *
      """
    Then the result should be, in any order:
      | a    | b    | x    |
      | (:A) | (:B) | (:X) |
    And no side effects

  Scenario: [3] Forwarding a relationship variable
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:T1]->(:X),
             ()-[:T2]->(:X),
             ()-[:T3]->()
      """
    When executing query:
      """
      MATCH ()-[r1]->(:X)
      LET __gql_with_scope_1 = 1, r2 = r1
      MATCH ()-[r2]->()
      RETURN r2 AS rel
      """
    Then the result should be, in any order:
      | rel   |
      | [:T1] |
      | [:T2] |
    And no side effects

  Scenario: [4] Forwarding a path variable
    Given an empty graph
    And having executed:
      """
      INSERT ()
      """
    When executing query:
      """
      MATCH p = (a)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_p = p
      RETURN p
      """
    Then the result should be, in any order:
      | p    |
      | <()> |
    And no side effects

  Scenario: [5] Forwarding null
    Given an empty graph
    When executing query:
      """
      OPTIONAL MATCH (a:Start)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a
      MATCH (a)-->(b)
      RETURN *
      """
    Then the result should be, in any order:
      | a | b |
    And no side effects

  Scenario: [6] Forwarding a node variable possibly null
    Given an empty graph
    And having executed:
      """
      INSERT (s:Single), (a:A {num: 42}),
             (b:B {num: 46}), (c:C)
      INSERT (s)-[:REL]->(a),
             (s)-[:REL]->(b),
             (a)-[:REL]->(c),
             (b)-[:LOOP]->(b)
      """
    When executing query:
      """
      OPTIONAL MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a_AS_a = a AS a
      MATCH (b:B)
      RETURN a, b
      """
    Then the result should be, in any order:
      | a              | b              |
      | (:A {num: 42}) | (:B {num: 46}) |
    And no side effects
