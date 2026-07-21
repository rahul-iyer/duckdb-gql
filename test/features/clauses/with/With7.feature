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
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/with/With7.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: With7 - WITH on WITH

  Scenario: [1] A simple pattern with one bound endpoint
    Given an empty graph
    And having executed:
      """
      INSERT (:A)-[:REL]->(:B)
      """
    When executing query:
      """
      MATCH (a:A)-[r:REL]->(b:B)
      LET __gql_with_scope_1 = 1, b = a, tmp = b, __gql_with_1_3_r_AS_r = r AS r
      LET __gql_with_scope_2 = 1, a = b, __gql_with_2_2_r = r
      LIMIT 1
      MATCH (a)-[r]->(b)
      RETURN a, r, b
      """
    Then the result should be, in any order:
      | a    | r      | b    |
      | (:A) | [:REL] | (:B) |
    And no side effects

  Scenario: [2] Multiple WITHs using a predicate and aggregation
    Given an empty graph
    And having executed:
      """
      INSERT (a {name: 'David'}),
             (b {name: 'Other'}),
             (c {name: 'NotOther'}),
             (d {name: 'NotOther2'}),
             (a)-[:REL]->(b),
             (a)-[:REL]->(c),
             (a)-[:REL]->(d),
             (b)-[:REL]->(),
             (b)-[:REL]->(),
             (c)-[:REL]->(),
             (c)-[:REL]->(),
             (d)-[:REL]->()
      """
    When executing query:
      """
      MATCH (david {name: 'David'})--(otherPerson)-->()
      LET __gql_with_scope_1 = 1, __gql_with_1_1_otherPerson = otherPerson, foaf = count(*)
      WHERE foaf > 1
      LET __gql_with_scope_2 = 1, __gql_with_2_1_otherPerson = otherPerson
      WHERE otherPerson.name <> 'NotOther'
      RETURN count(*)
      """
    Then the result should be, in any order:
      | count(*) |
      | 1        |
    And no side effects
