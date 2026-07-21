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
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/with-orderBy/WithOrderBy4.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: WithOrderBy4 - Order by in combination with projection and aliasing
# LIMIT is used in the following scenarios to surface the effects or WITH ... ORDER BY ...
# which are otherwise lost after the WITH clause according to Cypher semantics

  Scenario: [1] Sort by a projected expression
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num + num2 = 5
             (:A {num: 5, num2: 2}), //num + num2 = 7
             (:A {num: 9, num2: 0}), //num + num2 = 9
             (:A {num: 3, num2: 3}), //num + num2 = 6
             (:A {num: 7, num2: 1})  //num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2
        ORDER BY a.num + a.num2
        LIMIT 3
      RETURN a, sum
      """
    Then the result should be, in any order:
      | a                      | sum |
      | (:A {num: 1, num2: 4}) | 5   |
      | (:A {num: 3, num2: 3}) | 6   |
      | (:A {num: 5, num2: 2}) | 7   |
    And no side effects

  Scenario: [2] Sort by an alias of a projected expression
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num + num2 = 5
             (:A {num: 5, num2: 2}), //num + num2 = 7
             (:A {num: 9, num2: 0}), //num + num2 = 9
             (:A {num: 3, num2: 3}), //num + num2 = 6
             (:A {num: 7, num2: 1})  //num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2
        ORDER BY sum
        LIMIT 3
      RETURN a, sum
      """
    Then the result should be, in any order:
      | a                      | sum |
      | (:A {num: 1, num2: 4}) | 5   |
      | (:A {num: 3, num2: 3}) | 6   |
      | (:A {num: 5, num2: 2}) | 7   |
    And no side effects

  Scenario: [3] Sort by two projected expressions with order priority being different than projection order
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2, mod = a.num2 % 3
        ORDER BY a.num2 % 3, a.num + a.num2
        LIMIT 3
      RETURN a, sum, mod
      """
    Then the result should be, in any order:
      | a                      | sum | mod |
      | (:A {num: 3, num2: 3}) | 6   | 0   |
      | (:A {num: 9, num2: 0}) | 9   | 0   |
      | (:A {num: 1, num2: 4}) | 5   | 1   |
    And no side effects

  Scenario: [4] Sort by one projected expression and one alias of a projected expression with order priority being different than projection order
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2, mod = a.num2 % 3
        ORDER BY a.num2 % 3, sum
        LIMIT 3
      RETURN a, sum, mod
      """
    Then the result should be, in any order:
      | a                      | sum | mod |
      | (:A {num: 3, num2: 3}) | 6   | 0   |
      | (:A {num: 9, num2: 0}) | 9   | 0   |
      | (:A {num: 1, num2: 4}) | 5   | 1   |
    And no side effects

  Scenario: [5] Sort by one alias of a projected expression and one projected expression with order priority being different than projection order
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2, mod = a.num2 % 3
        ORDER BY mod, a.num + a.num2
        LIMIT 3
      RETURN a, sum, mod
      """
    Then the result should be, in any order:
      | a                      | sum | mod |
      | (:A {num: 3, num2: 3}) | 6   | 0   |
      | (:A {num: 9, num2: 0}) | 9   | 0   |
      | (:A {num: 1, num2: 4}) | 5   | 1   |
    And no side effects

  Scenario: [6] Sort by aliases of two projected expressions with order priority being different than projection order
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2, mod = a.num2 % 3
        ORDER BY mod, sum
        LIMIT 3
      RETURN a, sum, mod
      """
    Then the result should be, in any order:
      | a                      | sum | mod |
      | (:A {num: 3, num2: 3}) | 6   | 0   |
      | (:A {num: 9, num2: 0}) | 9   | 0   |
      | (:A {num: 1, num2: 4}) | 5   | 1   |
    And no side effects

  Scenario: [7] Sort by an alias of a projected expression where the alias shadows an existing variable
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, x = a.num2 % 3
      LET __gql_with_scope_2 = 1, __gql_with_2_1_a = a, x = a.num + a.num2
        ORDER BY x
        LIMIT 3
      RETURN a, x
      """
    Then the result should be, in any order:
      | a                      | x |
      | (:A {num: 1, num2: 4}) | 5 |
      | (:A {num: 3, num2: 3}) | 6 |
      | (:A {num: 5, num2: 2}) | 7 |
    And no side effects

  Scenario: [8] Sort by non-projected existing variable
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2
      LET __gql_with_scope_2 = 1, __gql_with_2_1_a = a, mod = a.num2 % 3
        ORDER BY sum
        LIMIT 3
      RETURN a, mod
      """
    Then the result should be, in any order:
      | a                      | mod |
      | (:A {num: 1, num2: 4}) | 1   |
      | (:A {num: 3, num2: 3}) | 0   |
      | (:A {num: 5, num2: 2}) | 2   |
    And no side effects

  Scenario: [9] Sort by an alias of a projected expression containing the variable shadowed by the alias
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, x = a.num2
      LET __gql_with_scope_2 = 1, x = x % 3
        ORDER BY x
        LIMIT 3
      RETURN x
      """
    Then the result should be, in any order:
      | x |
      | 0 |
      | 0 |
      | 1 |
    And no side effects

  Scenario: [10] Sort by a non-projected expression containing an alias of a projected expression containing the variable shadowed by the alias
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, x = a.num2
      LET __gql_with_scope_2 = 1, x = x % 3
        ORDER BY x * -1
        LIMIT 3
      RETURN x
      """
    Then the result should be, in any order:
      | x |
      | 2 |
      | 1 |
      | 1 |
    And no side effects

  Scenario: [11] Sort by an aggregate projection
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, mod = a.num2 % 3, sum = sum(a.num + a.num2)
        ORDER BY sum(a.num + a.num2)
        LIMIT 2
      RETURN mod, sum
      """
    Then the result should be, in any order:
      | mod | sum |
      | 2   | 7   |
      | 1   | 13  |
    And no side effects

  Scenario: [12] Sort by an aliased aggregate projection
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, mod = a.num2 % 3, sum = sum(a.num + a.num2)
        ORDER BY sum
        LIMIT 2
      RETURN mod, sum
      """
    Then the result should be, in any order:
      | mod | sum |
      | 2   | 7   |
      | 1   | 13  |
    And no side effects

  Scenario: [13] Fail on sorting by a non-projected aggregation on a variable
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, sum = a.num + a.num2
      LET __gql_with_scope_2 = 1, mod = a.num2 % 3, min = min(sum)
        ORDER BY sum(sum)
        LIMIT 2
      RETURN mod, min
      """
    Then a SyntaxError should be raised at compile time: UndefinedVariable

  Scenario: [14] Fail on sorting by a non-projected aggregation on an expression
    Given an empty graph
    And having executed:
      """
      INSERT (:A {num: 1, num2: 4}), //num2 % 3 = 1, num + num2 = 5
             (:A {num: 5, num2: 2}), //num2 % 3 = 2, num + num2 = 7
             (:A {num: 9, num2: 0}), //num2 % 3 = 0, num + num2 = 9
             (:A {num: 3, num2: 3}), //num2 % 3 = 0, num + num2 = 6
             (:A {num: 7, num2: 1})  //num2 % 3 = 1, num + num2 = 8
      """
    When executing query:
      """
      MATCH (a:A)
      LET __gql_with_scope_1 = 1, mod = a.num2 % 3, min = min(a.num + a.num2)
        ORDER BY sum(a.num + a.num2)
        LIMIT 2
      RETURN mod, min
      """
    Then a SyntaxError should be raised at compile time: UndefinedVariable

  Scenario: [15] Sort by an aliased aggregate projection does allow subsequent matching
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:T1 {id: 0}]->(:X),
             ()-[:T2 {id: 1}]->(:X),
             ()-[:T2 {id: 2}]->()
      """
    When executing query:
      """
      MATCH (a)-[r]->(b:X)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_a = a, __gql_with_1_2_r = r, __gql_with_1_3_b = b, c = count(*)
        ORDER BY c
      MATCH (a)-[r]->(b)
      RETURN r AS rel
        ORDER BY rel.id
      """
    Then the result should be, in order:
      | rel           |
      | [:T1 {id: 0}] |
      | [:T2 {id: 1}] |
    And no side effects

  Scenario: [16] Handle constants and parameters inside an order by item which contains an aggregation expression
    Given an empty graph
    And parameters are:
      | age | 38 |
    When executing query:
      """
      MATCH (person)
      LET __gql_with_scope_1 = 1, avgAge = avg(person.age)
      ORDER BY $age + avg(person.age) - 1000
      RETURN avgAge
      """
    Then the result should be, in any order:
      | avgAge |
      | null   |
    And no side effects

  Scenario: [17] Handle projected variables inside an order by item which contains an aggregation expression
    Given an empty graph
    When executing query:
      """
      MATCH (me: Person)--(you: Person)
      LET __gql_with_scope_1 = 1, age = me.age, cnt = count(you.age)
      ORDER BY age, age + count(you.age)
      RETURN age
      """
    Then the result should be, in any order:
      | age |
    And no side effects

  Scenario: [18]  Handle projected property accesses inside an order by item which contains an aggregation expression
    Given an empty graph
    When executing query:
      """
      MATCH (me: Person)--(you: Person)
      LET __gql_with_scope_1 = 1, age = me.age, cnt = count(you.age)
      ORDER BY me.age + count(you.age)
      RETURN age
      """
    Then the result should be, in any order:
      | age |
    And no side effects

  Scenario: [19] Fail if not projected variables are used inside an order by item which contains an aggregation expression
    Given an empty graph
    When executing query:
      """
      MATCH (me: Person)--(you: Person)
      LET __gql_with_scope_1 = 1, agg = count(you.age)
      ORDER BY me.age + count(you.age)
      RETURN *
      """
    Then a SyntaxError should be raised at compile time: UndefinedVariable

  Scenario: [20] Fail if more complex expressions, even if projected, are used inside an order by item which contains an aggregation expression
    Given an empty graph
    When executing query:
      """
      MATCH (me: Person)--(you: Person)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_me_age___you_age = me.age + you.age, cnt = count(*)
      ORDER BY me.age + you.age + count(*)
      RETURN *
      """
    Then a SyntaxError should be raised at compile time: AmbiguousAggregationExpression
