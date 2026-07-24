#pragma once

#include "gql_ir.hpp"

#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class GqlProcedureArgumentMode : uint8_t { CONFIGURATION, INPUT };

struct GqlProcedureArgumentDefinition {
  string name;
  GqlType type;
  GqlProcedureArgumentMode mode;
  bool optional = false;
};

struct GqlProcedureOutputDefinition {
  string name;
  GqlType type;
};

struct GqlProcedureDefinition {
  string procedure_namespace;
  string name;
  GqlProcedureInputMode input_mode;
  vector<GqlProcedureArgumentDefinition> arguments;
  vector<GqlProcedureOutputDefinition> outputs;
  bool read_only = true;
  bool blocking = true;
};

const GqlProcedureDefinition *
GqlFindProcedure(const string &procedure_namespace, const string &name);
TableFunction GqlAlgorithmCallFunction();
TableFunction GqlAlgorithmResultFunction();

TableFunction GqlBfsFunction();
TableFunction GqlDfsFunction();
TableFunction GqlSsspFunction();
TableFunction GqlPageRankFunction();
TableFunction GqlWccFunction();
TableFunction GqlSccFunction();
TableFunction GqlTriangleCountFunction();
TableFunction GqlLccFunction();
TableFunction GqlDegreeFunction();
TableFunction GqlClosenessFunction();

} // namespace duckdb
