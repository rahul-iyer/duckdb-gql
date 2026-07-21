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
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/with-where/WithWhere1.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: WithWhere1 - Filter single variable

  Scenario: [1] Filter node with property predicate on a single variable with multiple bindings
    Given an empty graph
    And having executed:
      """
      INSERT ({name: 'A'}),
             ({name: 'B'}),
             ({name: 'C'})
      """
    When executing query:
      """
      MATCH (a)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a
      WHERE a.name = 'B'
      RETURN a
      """
    Then the result should be, in any order:
      | a             |
      | ({name: 'B'}) |
    And no side effects

  Scenario: [2] Filter node with property predicate on a single variable with multiple distinct bindings
    Given an empty graph
    And having executed:
      """
      INSERT ({name2: 'A'}),
             ({name2: 'A'}),
             ({name2: 'B'})
      """
    When executing query:
      """
      MATCH (a)
      LET __gql_with_scope_1 = 1, name = a.name2
      WHERE a.name2 = 'B'
      RETURN *
      """
    Then the result should be, in any order:
      | name |
      | 'B'  |
    And no side effects

  Scenario: [3] Filter for an unbound relationship variable
    Given an empty graph
    And having executed:
      """
      INSERT (a:A), (b:B {id: 1}), (:B {id: 2})
      INSERT (a)-[:T]->(b)
      """
    When executing query:
      """
      MATCH (a:A), (other:B)
      OPTIONAL MATCH (a)-[r]->(other)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_other = other
      FILTER WHERE r IS NULL
      RETURN other
      """
    Then the result should be, in any order:
      | other        |
      | (:B {id: 2}) |
    And no side effects

  Scenario: [4] Filter for an unbound node variable
    Given an empty graph
    And having executed:
      """
      INSERT (a:A), (b:B {id: 1}), (:B {id: 2})
      INSERT (a)-[:T]->(b)
      """
    When executing query:
      """
      MATCH (other:B)
      OPTIONAL MATCH (a)-[r]->(other)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_other = other
      FILTER WHERE a IS NULL
      RETURN other
      """
    Then the result should be, in any order:
      | other        |
      | (:B {id: 2}) |
    And no side effects
