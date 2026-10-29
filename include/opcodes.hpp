#pragma once
#include <string>
#include <vector>

namespace csystem
{
// Types describe values in the compiled script. Variable means a reference,
// rather than the current value of that variable.
enum class OperandType
{
    Integer,
    Variable,
    IntegerOrVariable,
    Text
};

struct ParameterDefinition
{
    std::string name;
    OperandType type;
};

struct OpcodeDefinition
{
    int code;
    std::string name;
    std::vector<ParameterDefinition> parameters;
    std::string description;
};

const std::vector<OpcodeDefinition>& native_opcodes();
const std::vector<OpcodeDefinition>& legacy_opcode_variants();
const std::vector<OpcodeDefinition>& sprite_opcodes();
const std::vector<OpcodeDefinition>& legacy_sprite_variants();
}
