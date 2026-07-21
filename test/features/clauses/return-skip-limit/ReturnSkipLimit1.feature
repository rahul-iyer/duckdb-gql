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
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/return-skip-limit/ReturnSkipLimit1.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: ReturnSkipLimit1 - Skip

  Scenario: [1] Start the result from the second row
    Given an empty graph
    And having executed:
      """
      INSERT ({name: 'A'}),
        ({name: 'B'}),
        ({name: 'C'}),
        ({name: 'D'}),
        ({name: 'E'})
      """
    When executing query:
      """
      MATCH (n)
      RETURN n
      ORDER BY n.name ASC
      OFFSET 2
      """
    Then the result should be, in order:
      | n             |
      | ({name: 'C'}) |
      | ({name: 'D'}) |
      | ({name: 'E'}) |
    And no side effects

  Scenario: [2] Start the result from the second row by param
    Given an empty graph
    And having executed:
      """
      INSERT ({name: 'A'}),
        ({name: 'B'}),
        ({name: 'C'}),
        ({name: 'D'}),
        ({name: 'E'})
      """
    And parameters are:
      | skipAmount | 2 |
    When executing query:
      """
      MATCH (n)
      RETURN n
      ORDER BY n.name ASC
      OFFSET $skipAmount
      """
    Then the result should be, in order:
      | n             |
      | ({name: 'C'}) |
      | ({name: 'D'}) |
      | ({name: 'E'}) |
    And no side effects

  Scenario: [3] SKIP with an expression that does not depend on variables
    Given any graph
    And having executed:
      """
      FOR i IN range(1, 10)
      INSERT ({nr: i})
      """
    When executing query:
      """
      MATCH (n)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_n = n
      OFFSET toInteger(rand()*9)
      LET __gql_with_scope_2 = 1, count = count(*)
      RETURN count > 0 AS nonEmpty
      """
    Then the result should be, in any order:
      | nonEmpty |
      | true     |
    And no side effects

  Scenario: [4] Accept skip zero
    Given any graph
    When executing query:
      """
      MATCH (n)
      WHERE 1 = 0
      RETURN n OFFSET 0
      """
    Then the result should be, in any order:
      | n |
    And no side effects

  Scenario: [5] SKIP with an expression that depends on variables should fail
    Given any graph
    When executing query:
      """
      MATCH (n) RETURN n OFFSET n.count
      """
    Then a SyntaxError should be raised at compile time: NonConstantExpression

  Scenario: [6] Negative parameter for SKIP should fail
    Given any graph
    And having executed:
      """
      INSERT (s:Person {name: 'Steven'}),
             (c:Person {name: 'Craig'})
      """
    And parameters are:
      | _skip | -1 |
    When executing query:
      """
      MATCH (p:Person)
      RETURN p.name AS name
      OFFSET $_skip
      """
    Then a SyntaxError should be raised at runtime: NegativeIntegerArgument

  Scenario: [7] Negative SKIP should fail
    Given any graph
    And having executed:
      """
      INSERT (s:Person {name: 'Steven'}),
             (c:Person {name: 'Craig'})
      """
    When executing query:
      """
      MATCH (p:Person)
      RETURN p.name AS name
      OFFSET -1
      """
    Then a SyntaxError should be raised at compile time: NegativeIntegerArgument

  Scenario: [8] Floating point parameter for SKIP should fail
    Given any graph
    And having executed:
      """
      INSERT (s:Person {name: 'Steven'}),
             (c:Person {name: 'Craig'})
      """
    And parameters are:
      | _limit | 1.5 |
    When executing query:
      """
      MATCH (p:Person)
      RETURN p.name AS name
      OFFSET $_limit
      """
    Then a SyntaxError should be raised at runtime: InvalidArgumentType

  Scenario: [9] Floating point SKIP should fail
    Given any graph
    And having executed:
      """
      INSERT (s:Person {name: 'Steven'}),
             (c:Person {name: 'Craig'})
      """
    When executing query:
      """
      MATCH (p:Person)
      RETURN p.name AS name
      OFFSET 1.5
      """
    Then a SyntaxError should be raised at compile time: InvalidArgumentType

  Scenario: [10] Fail when using non-constants in SKIP
    Given any graph
    When executing query:
      """
      MATCH (n)
      RETURN n
        OFFSET n.count
      """
    Then a SyntaxError should be raised at compile time: NonConstantExpression

  Scenario: [11] Fail when using negative value in SKIP
    Given any graph
    When executing query:
      """
      MATCH (n)
      RETURN n
        OFFSET -1
      """
    Then a SyntaxError should be raised at compile time: NegativeIntegerArgument
