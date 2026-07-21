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
# Source: https://github.com/opencypher/openCypher/blob/677cbafabb8c3c5eed458fd3b1ec0daec8d67d23/tck/features/clauses/delete/Delete6.feature
# Modified by duckdb-gql: query text uses the mechanical GQL mappings documented in test/features/README.md; scenario semantics remain unverified.

Feature: Delete6 - Persistence of delete clause side effects

  Scenario: [1] Limiting to zero results after deleting nodes affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 42})
      """
    When executing query:
      """
      MATCH (n:N)
      DELETE n
      RETURN 42 AS num
      LIMIT 0
      """
    Then the result should be, in any order:
      | num |
    And the side effects should be:
      | -nodes      | 1 |
      | -labels     | 1 |
      | -properties | 1 |

  Scenario: [2] Skipping all results after deleting nodes affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 42})
      """
    When executing query:
      """
      MATCH (n:N)
      DELETE n
      RETURN 42 AS num
      OFFSET 1
      """
    Then the result should be, in any order:
      | num |
    And the side effects should be:
      | -nodes      | 1 |
      | -labels     | 1 |
      | -properties | 1 |

  Scenario: [3] Skipping and limiting to a few results after deleting nodes affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 1})
      INSERT (:N {num: 2})
      INSERT (:N {num: 3})
      INSERT (:N {num: 4})
      INSERT (:N {num: 5})
      """
    When executing query:
      """
      MATCH (n:N)
      DELETE n
      RETURN 42 AS num
      OFFSET 2 LIMIT 2
      """
    Then the result should be, in any order:
      | num |
      | 42  |
      | 42  |
    And the side effects should be:
      | -nodes      | 5 |
      | -labels     | 1 |
      | -properties | 5 |

  Scenario: [4] Skipping zero results and limiting to all results after deleting nodes does not affect the result set nor the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 1})
      INSERT (:N {num: 2})
      INSERT (:N {num: 3})
      INSERT (:N {num: 4})
      INSERT (:N {num: 5})
      """
    When executing query:
      """
      MATCH (n:N)
      DELETE n
      RETURN 42 AS num
      OFFSET 0 LIMIT 5
      """
    Then the result should be, in any order:
      | num |
      | 42  |
      | 42  |
      | 42  |
      | 42  |
      | 42  |
    And the side effects should be:
      | -nodes      | 5 |
      | -labels     | 1 |
      | -properties | 5 |

  Scenario: [5] Filtering after deleting nodes affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 1})
      INSERT (:N {num: 2})
      INSERT (:N {num: 3})
      INSERT (:N {num: 4})
      INSERT (:N {num: 5})
      """
    When executing query:
      """
      MATCH (n:N)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_n = n, num = n.num
      DELETE n
      LET __gql_with_scope_2 = 1, __gql_with_2_1_num = num
      WHERE num % 2 = 0
      RETURN num
      """
    Then the result should be, in any order:
      | num |
      | 2   |
      | 4   |
    And the side effects should be:
      | -nodes      | 5 |
      | -labels     | 1 |
      | -properties | 5 |

  Scenario: [6] Aggregating in `RETURN` after deleting nodes affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 1})
      INSERT (:N {num: 2})
      INSERT (:N {num: 3})
      INSERT (:N {num: 4})
      INSERT (:N {num: 5})
      """
    When executing query:
      """
      MATCH (n:N)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_n = n, num = n.num
      DELETE n
      RETURN sum(num) AS sum
      """
    Then the result should be, in any order:
      | sum |
      | 15  |
    And the side effects should be:
      | -nodes      | 5 |
      | -labels     | 1 |
      | -properties | 5 |

  Scenario: [7] Aggregating in `WITH` after deleting nodes affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT (:N {num: 1})
      INSERT (:N {num: 2})
      INSERT (:N {num: 3})
      INSERT (:N {num: 4})
      INSERT (:N {num: 5})
      """
    When executing query:
      """
      MATCH (n:N)
      LET __gql_with_scope_1 = 1, __gql_with_1_1_n = n, num = n.num
      DELETE n
      LET __gql_with_scope_2 = 1, sum = sum(num)
      RETURN sum
      """
    Then the result should be, in any order:
      | sum |
      | 15  |
    And the side effects should be:
      | -nodes      | 5 |
      | -labels     | 1 |
      | -properties | 5 |

  Scenario: [8] Limiting to zero results after deleting relationships affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[r:R {num: 42}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      DELETE r
      RETURN 42 AS num
      LIMIT 0
      """
    Then the result should be, in any order:
      | num |
    And the side effects should be:
      | -relationships | 1 |
      | -properties    | 1 |

  Scenario: [9] Skipping all results after deleting relationships affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[r:R {num: 42}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      DELETE r
      RETURN 42 AS num
      OFFSET 1
      """
    Then the result should be, in any order:
      | num |
    And the side effects should be:
      | -relationships | 1 |
      | -properties    | 1 |

  Scenario: [10] Skipping and limiting to a few results after deleting relationships affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:R {num: 1}]->()
      INSERT ()-[:R {num: 2}]->()
      INSERT ()-[:R {num: 3}]->()
      INSERT ()-[:R {num: 4}]->()
      INSERT ()-[:R {num: 5}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      DELETE r
      RETURN 42 AS num
      OFFSET 2 LIMIT 2
      """
    Then the result should be, in any order:
      | num |
      | 42  |
      | 42  |
    And the side effects should be:
      | -relationships | 5 |
      | -properties    | 5 |

  Scenario: [11] Skipping zero result and limiting to all results after deleting relationships does not affect the result set nor the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:R {num: 1}]->()
      INSERT ()-[:R {num: 2}]->()
      INSERT ()-[:R {num: 3}]->()
      INSERT ()-[:R {num: 4}]->()
      INSERT ()-[:R {num: 5}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      DELETE r
      RETURN 42 AS num
      OFFSET 0 LIMIT 5
      """
    Then the result should be, in any order:
      | num |
      | 42  |
      | 42  |
      | 42  |
      | 42  |
      | 42  |
    And the side effects should be:
      | -relationships | 5 |
      | -properties    | 5 |

  Scenario: [12] Filtering after deleting relationships affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:R {num: 1}]->()
      INSERT ()-[:R {num: 2}]->()
      INSERT ()-[:R {num: 3}]->()
      INSERT ()-[:R {num: 4}]->()
      INSERT ()-[:R {num: 5}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      LET __gql_with_scope_1 = 1, __gql_with_1_1_r = r, num = r.num
      DELETE r
      LET __gql_with_scope_2 = 1, __gql_with_2_1_num = num
      WHERE num % 2 = 0
      RETURN num
      """
    Then the result should be, in any order:
      | num |
      | 2   |
      | 4   |
    And the side effects should be:
      | -relationships | 5 |
      | -properties    | 5 |

  Scenario: [13] Aggregating in `RETURN` after deleting relationships affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:R {num: 1}]->()
      INSERT ()-[:R {num: 2}]->()
      INSERT ()-[:R {num: 3}]->()
      INSERT ()-[:R {num: 4}]->()
      INSERT ()-[:R {num: 5}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      LET __gql_with_scope_1 = 1, __gql_with_1_1_r = r, num = r.num
      DELETE r
      RETURN sum(num) AS sum
      """
    Then the result should be, in any order:
      | sum |
      | 15  |
    And the side effects should be:
      | -relationships | 5 |
      | -properties    | 5 |

  Scenario: [14] Aggregating in `WITH` after deleting relationships affects the result set but not the side effects
    Given an empty graph
    And having executed:
      """
      INSERT ()-[:R {num: 1}]->()
      INSERT ()-[:R {num: 2}]->()
      INSERT ()-[:R {num: 3}]->()
      INSERT ()-[:R {num: 4}]->()
      INSERT ()-[:R {num: 5}]->()
      """
    When executing query:
      """
      MATCH ()-[r:R]->()
      LET __gql_with_scope_1 = 1, __gql_with_1_1_r = r, num = r.num
      DELETE r
      LET __gql_with_scope_2 = 1, sum = sum(num)
      RETURN sum
      """
    Then the result should be, in any order:
      | sum |
      | 15  |
    And the side effects should be:
      | -relationships | 5 |
      | -properties    | 5 |
