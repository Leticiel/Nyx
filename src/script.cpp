#include "opcodes.hpp"
#include "tool.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace csystem
{
Json catalog()
{
    Json result = Json::array();
    for (const auto& d : native_opcodes())
    {
        Json args = Json::array();
        for (const auto& p : d.parameters)
        {
            args.push_back(p.name);
        }
        result.push_back({{"name", d.name}, {"parameters", args}, {"description", d.description}});
    }
    for (const auto& d : legacy_opcode_variants())
    {
        Json args = Json::array();
        for (const auto& parameter : d.parameters)
        {
            args.push_back(parameter.name);
        }
        result.push_back({{"name", d.name}, {"parameters", args}, {"description", d.description}});
    }
    return result;
}

namespace
{
const std::map<std::string, int32_t> transition_constants = {{"transition.immediate", 0},
                                                             {"transition.crossfade_500ms", 2},
                                                             {"transition.crossfade_1000ms", 3},
                                                             {"transition.crossfade_2000ms", 4},
                                                             {"transition.blinds_left_to_right", 9},
                                                             {"transition.blinds_bottom_to_top", 11},
                                                             {"transition.blinds_top_to_bottom", 12},
                                                             {"transition.mosaic", 13},
                                                             {"transition.deferred", 9999}};

struct Spec
{
    int code;
    std::string name;
    std::vector<std::string> params, kinds;
};

struct Specs
{
    std::map<int, Spec> commands, objects;
    std::map<std::pair<int, size_t>, Spec> variants, object_variants;
    std::map<std::string, std::pair<bool, Spec>> names;

    Specs()
    {
        auto add = [&](const auto& definitions, bool obj, bool variant = false)
        {
            for (const auto& d : definitions)
            {
                Spec x;
                x.code = d.code;
                x.name = d.name;
                for (const auto& p : d.parameters)
                {
                    x.params.push_back(p.name);
                    switch (p.type)
                    {
                    case OperandType::Integer:
                        x.kinds.push_back("N");
                        break;
                    case OperandType::Variable:
                        x.kinds.push_back("A");
                        break;
                    case OperandType::IntegerOrVariable:
                        x.kinds.push_back("NA");
                        break;
                    case OperandType::Text:
                        x.kinds.push_back("T");
                        break;
                    }
                }
                if (variant)
                {
                    (obj ? object_variants : variants)[{x.code, x.params.size()}] = x;
                }
                else
                {
                    (obj ? objects : commands)[x.code] = x;
                }
                if (!obj || !variant)
                {
                    names[x.name] = {obj, x};
                }
            }
        };
        add(native_opcodes(), false);
        add(sprite_opcodes(), true);
        add(legacy_opcode_variants(), false, true);
        add(legacy_sprite_variants(), true, true);
    }
};

const Specs& specs()
{
    static Specs s;
    return s;
}

Operand number(int32_t v)
{
    Operand a;
    a.value = v;
    return a;
}

Record command(int code, std::vector<Operand> args = {}, char marker = '#')
{
    Record r;
    r.opcode = code;
    r.marker = marker;
    r.operands = std::move(args);
    return r;
}

void validate(const Record& r)
{
    if (!r.typed || r.tag != 'M' || r.marker == 'L')
    {
        return;
    }
    auto& table = r.marker == '$' ? specs().objects : specs().commands;
    int code = r.opcode;
    size_t start = 0;
    if (r.marker == '$')
    {
        if (r.operands.empty() || r.operands[0].type != 'N')
        {
            throw Error("sprite command needs a literal subtype");
        }
        code = r.operands[0].value;
        start = 1;
    }
    auto it = table.find(code);
    if (it == table.end())
    {
        return;
    }
    const auto& variants = r.marker == '$' ? specs().object_variants : specs().variants;
    const auto variant = variants.find({code, r.operands.size() - start});
    const auto& s = variant != variants.end() ? variant->second : it->second;
    size_t count = s.params.size();
    if (r.marker == '$' && r.operands.size() > 1 && r.operands[1].type == 'N' &&
        ((code == 5 && r.operands[1].value == 115) || (code == 7 && r.operands[1].value != 0)))
    {
        count = 1;
    }
    if (r.marker == '#' && code == 181)
    {
        auto& a = r.operands;
        if (a.empty())
        {
            throw Error("missing callback selector");
        }
        for (auto& o : a)
        {
            if (o.type != 'N')
            {
                throw Error("callback arguments must be literal");
            }
        }
        if (a[0].value == 0)
        {
            if (a.size() < 2 || a[1].value < 0 || a.size() != 2 + uint64_t(a[1].value) * 2)
            {
                throw Error("callback count mismatch");
            }
        }
        else if (a.size() != 1)
        {
            throw Error("callback selector arity");
        }
        return;
    }
    bool tail = r.marker == '#' && (code == 49 || code == 68 || code == 69);
    if (r.operands.size() < start + count || (!tail && r.operands.size() != start + count))
    {
        throw Error(s.name + ": incorrect operand count");
    }
    for (size_t i = 0; i < count; i++)
    {
        if (s.kinds[i].find(r.operands[start + i].type) == std::string::npos)
        {
            throw Error(s.name + ": incorrect operand type for " + s.params[i]);
        }
    }
}

std::map<size_t, std::vector<size_t>> targets(const std::vector<Record>& rows)
{
    std::map<size_t, std::vector<size_t>> out;
    std::pair<int, int> state{-1, -1};
    std::vector<std::pair<std::pair<int, int>, std::optional<std::pair<int, int>>>> stack;
    for (size_t i = 0; i < rows.size(); i++)
    {
        auto& r = rows[i];
        if (r.tag != 'M' || r.marker != '#' || !r.typed)
        {
            continue;
        }
        int c = r.opcode;
        if (c == 1 || c == 122)
        {
            out[i] = {0};
        }
        if (c == 50)
        {
            out[i] = {0, 1, 2, 3};
        }
        if (c == 100)
        {
            out[i] = {0, 1};
        }
        if (((c == 18 || c == 22) && state.first == 2) ||
            ((c == 41 || c == 92 || c == 128) && state.second == 2))
        {
            out[i] = {0};
        }
        if (c == 8 || c == 139)
        {
            stack.push_back({state, {}});
        }
        else if (c == 9 && !stack.empty())
        {
            stack.back().second = state;
            state = stack.back().first;
        }
        else if (c == 10 && !stack.empty())
        {
            auto [before, branch] = stack.back();
            stack.pop_back();
            auto other = branch.value_or(before);
            state = {state.first == other.first ? state.first : -1,
                     state.second == other.second ? state.second : -1};
        }
        else if (c == 17 || c == 21)
        {
            state.first = c == 17 ? 1 : 2;
        }
        else if (c == 40 || c == 91 || c == 127)
        {
            state.second = c == 91 ? 2 : 1;
        }
        else if (c == 19 || c == 23)
        {
            state.first = -1;
        }
        else if (c == 42 || c == 93 || c == 129)
        {
            state.second = -1;
        }
        else if (c == 0 || c == 1 || c == 2 || c == 11 || c == 54 || c == 62 || c == 74 ||
                 c == 76 || c == 126 || c == 161 || c == 188)
        {
            state = {-1, -1};
        }
    }
    return out;
}

std::string quote(const std::string& s)
{
    return Json(s).dump(-1, ' ', false, Json::error_handler_t::strict);
}

struct Token
{
    std::string text;
    size_t line;
    bool string = false;
};

std::vector<Token> lex(const std::string& s)
{
    std::vector<Token> ts;
    size_t i = 0, line = 1;
    while (i < s.size())
    {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (c == '\n')
            {
                line++;
            }
            i++;
            continue;
        }
        if (c == '#' || (c == '/' && i + 1 < s.size() && s[i + 1] == '/'))
        {
            while (i < s.size() && s[i] != '\n')
            {
                i++;
            }
            continue;
        }
        size_t ln = line;
        if (c == '"')
        {
            size_t a = i++;
            bool escape = false;
            for (; i < s.size(); i++)
            {
                if (s[i] == '\n')
                {
                    line++;
                }
                if (!escape && s[i] == '"')
                {
                    i++;
                    break;
                }
                if (!escape && s[i] == '\\')
                {
                    escape = true;
                }
                else
                {
                    escape = false;
                }
            }
            try
            {
                ts.push_back({Json::parse(s.substr(a, i - a)).get<std::string>(), ln, true});
            }
            catch (const std::exception&)
            {
                throw Error("invalid string at line " + std::to_string(ln));
            }
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t a = i++;
            const bool numeric = std::isdigit(static_cast<unsigned char>(c));
            while (
                i < s.size() &&
                (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.' ||
                 (numeric && (s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
            {
                i++;
            }
            ts.push_back({s.substr(a, i - a), ln});
            continue;
        }
        std::string op(1, c);
        i++;
        if (i < s.size() && (c == '!' || c == '<' || c == '>' || c == '=') && s[i] == '=')
        {
            op += '=';
            i++;
        }
        ts.push_back({op, ln});
    }
    ts.push_back({"<eof>", line});
    return ts;
}

struct Expr
{
    std::string name;
    Json literal;
    std::vector<Expr> args;
    std::vector<std::string> keys;
    bool call = false, is_literal = false;
};

class Parser
{
    std::vector<Token> ts;
    size_t at = 0;
    std::map<std::string, int32_t> constants;
    ResourceResolver resource_resolver;
    uint32_t scene = 0;
    std::vector<Record> rows;
    std::map<std::string, size_t> labels;

    struct Pending
    {
        size_t row, arg;
        std::string label;
    };

    std::vector<Pending> pending;

    [[noreturn]] void fail(const std::string& m)
    {
        throw Error("line " + std::to_string(ts[at].line) + ": " + m);
    }

    bool is(const std::string& s)
    {
        return !ts[at].string && ts[at].text == s;
    }

    bool eat(const std::string& s)
    {
        if (is(s))
        {
            at++;
            return true;
        }
        return false;
    }

    void need(const std::string& s)
    {
        if (!eat(s))
        {
            fail("expected '" + s + "', got '" + ts[at].text + "'");
        }
    }

    std::string identifier()
    {
        if (ts[at].string || ts[at].text.empty() ||
            !(std::isalpha(static_cast<unsigned char>(ts[at].text[0])) || ts[at].text[0] == '_'))
        {
            fail("expected name");
        }
        return ts[at++].text;
    }

    Expr value()
    {
        Expr e;
        if (is("<eof>"))
        {
            fail("unexpected end of script");
        }
        if (ts[at].string)
        {
            e.literal = ts[at++].text;
            e.is_literal = true;
            return e;
        }
        if (eat("["))
        {
            Json a = Json::array();
            if (!eat("]"))
            {
                do
                {
                    a.push_back(json_value(value()));
                } while (eat(","));
                need("]");
            }
            e.literal = a;
            e.is_literal = true;
            return e;
        }
        if (eat("{"))
        {
            Json j = Json::object();
            if (!eat("}"))
            {
                do
                {
                    if (!ts[at].string)
                    {
                        fail("JSON key must be quoted");
                    }
                    auto k = ts[at++].text;
                    need(":");
                    j[k] = json_value(value());
                } while (eat(","));
                need("}");
            }
            e.literal = j;
            e.is_literal = true;
            return e;
        }
        auto t = ts[at++].text;
        if (t == "-" || t == "+")
        {
            if (ts[at].string || ts[at].text.empty() ||
                !std::isdigit(static_cast<unsigned char>(ts[at].text[0])))
            {
                fail("expected a number after the sign");
            }
            t = (t == "-" ? "-" : "") + ts[at++].text;
        }
        if (t == "true" || t == "false" || t == "null" ||
            (!t.empty() &&
             (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-' || t[0] == '+')))
        {
            try
            {
                e.literal = Json::parse(t);
                e.is_literal = true;
                return e;
            }
            catch (...)
            {
                fail("invalid literal " + t);
            }
        }
        e.name = t;
        if (eat("("))
        {
            e.call = true;
            if (!eat(")"))
            {
                do
                {
                    std::string key;
                    if (at + 1 < ts.size() && !ts[at].string && ts[at + 1].text == "=")
                    {
                        key = identifier();
                        need("=");
                    }
                    e.keys.push_back(key);
                    e.args.push_back(value());
                } while (eat(","));
                need(")");
            }
        }
        return e;
    }

    Json json_value(const Expr& e)
    {
        if (!e.is_literal)
        {
            fail("expected JSON value");
        }
        return e.literal;
    }

    int64_t integer(const Expr& e)
    {
        if (e.is_literal && e.literal.is_number_integer())
        {
            return e.literal.get<int64_t>();
        }
        if (!e.call && constants.contains(e.name))
        {
            return constants.at(e.name);
        }
        if (!e.call && transition_constants.contains(e.name))
        {
            return transition_constants.at(e.name);
        }
        fail("expected integer or declared constant");
    }

    int32_t i32(const Expr& e)
    {
        auto n = integer(e);
        if (n < INT32_MIN || n > INT32_MAX)
        {
            fail("signed integer overflow");
        }
        return int32_t(n);
    }

    uint32_t u32(const Expr& e)
    {
        auto n = integer(e);
        if (n < 0 || uint64_t(n) > UINT32_MAX)
        {
            fail("unsigned integer overflow");
        }
        return uint32_t(n);
    }

    std::string string(const Expr& e)
    {
        if (e.is_literal && e.literal.is_string())
        {
            return e.literal.get<std::string>();
        }
        fail("expected string");
    }

    Operand operand(const Expr& e, size_t arg)
    {
        if (!e.call && e.name.starts_with("v.variable_"))
        {
            auto s = e.name.substr(11);
            size_t p = 0;
            auto n = std::stoll(s, &p);
            if (p != s.size() || n < INT32_MIN || n > INT32_MAX)
            {
                fail("invalid variable");
            }
            auto o = number(int32_t(n));
            o.type = 'A';
            return o;
        }
        if (e.call && (e.name == "var" || e.name == "ref" || e.name == "text.literal"))
        {
            if (e.name == "var" && e.args.size() == 1)
            {
                auto o = number(i32(e.args[0]));
                o.type = 'A';
                return o;
            }
            if (e.name == "ref" && e.args.size() == 1)
            {
                pending.push_back({rows.size(), arg, string(e.args[0])});
                return number(0);
            }
            if (e.name == "text.literal" && e.args.size() == 2)
            {
                Operand o;
                o.type = 'T';
                o.text_id = u32(e.args[0]);
                o.text = utf16(string(e.args[1]));
                return o;
            }
            fail("invalid operand helper");
        }
        if (e.is_literal && e.literal.is_string())
        {
            Operand o;
            o.type = 'T';
            o.text_id = checked32(uint64_t(scene) * 100000 + rows.size() + 1);
            o.text = utf16(string(e));
            return o;
        }
        return number(i32(e));
    }

    void emit(Record r)
    {
        validate(r);
        rows.push_back(std::move(r));
    }

    std::vector<Expr>
    arguments(const Expr& e, const std::vector<std::string>& params, bool defaults = false)
    {
        bool named = std::any_of(e.keys.begin(),
                                 e.keys.end(),
                                 [](auto& s)
                                 {
                                     return !s.empty();
                                 });
        if (!named)
        {
            return e.args;
        }
        std::vector<std::optional<Expr>> slots(params.size());
        size_t p = 0;
        bool saw_named = false;
        for (size_t i = 0; i < e.args.size(); i++)
        {
            size_t j;
            if (e.keys[i].empty())
            {
                if (saw_named)
                {
                    fail("positional argument after named argument");
                }
                j = p++;
            }
            else
            {
                saw_named = true;
                auto it = std::find(params.begin(), params.end(), e.keys[i]);
                if (it == params.end())
                {
                    fail("unknown argument " + e.keys[i]);
                }
                j = it - params.begin();
            }
            if (j >= slots.size() || slots[j])
            {
                fail("duplicate/excess argument");
            }
            slots[j] = e.args[i];
        }
        std::vector<Expr> out;
        for (size_t i = 0; i < slots.size(); i++)
        {
            if (!slots[i] && defaults &&
                (params[i] == "mode" || params[i] == "layout" || params[i] == "transition" ||
                 params[i] == "event_flag"))
            {
                Expr n;
                n.is_literal = true;
                n.literal = params[i] == "event_flag" ? 1 : 0;
                slots[i] = n;
            }
            if (!slots[i])
            {
                fail("missing argument " + params[i]);
            }
            out.push_back(*slots[i]);
        }
        return out;
    }

    void call(const Expr& e)
    {
        if (!e.call)
        {
            fail("expected instruction call");
        }
        if (e.name == "flow.label" || e.name == "flow.mark")
        {
            if (e.args.empty() || e.args.size() > 2)
            {
                fail("invalid label arguments");
            }
            auto label = string(e.args[0]);
            if (!labels.emplace(label, rows.size()).second)
            {
                fail("duplicate label " + label);
            }
            if (e.name == "flow.label")
            {
                emit(command(e.args.size() == 2 ? i32(e.args[1]) : 0, {}, 'L'));
            }
            return;
        }
        if (e.name == "dialogue.say")
        {
            auto args = arguments(e,
                                  e.args.size() == 1 ? std::vector<std::string>{"text"}
                                                     : std::vector<std::string>{"text", "id"});
            if (args.empty() || args.size() > 2)
            {
                fail("dialogue.say(text [, id])");
            }
            Record r;
            r.tag = 'T';
            r.text = utf16(string(args[0]));
            r.text_id = args.size() == 2 ? u32(args[1])
                                         : checked32(uint64_t(scene) * 100000 + rows.size() + 1);
            emit(r);
            return;
        }
        if (e.name == "binary.record")
        {
            if (e.args.size() != 1)
            {
                fail("binary.record needs body hex");
            }
            auto body = unhex(string(e.args[0]));
            Bytes framed;
            put(framed, checked32(body.size()));
            append(framed, body);
            auto rr = decode_records(framed);
            if (rr.size() != 1)
            {
                fail("invalid raw record");
            }
            rows.push_back(rr[0]);
            return;
        }
        if (e.name == "native.command")
        {
            if (e.args.empty())
            {
                fail("native.command needs an opcode");
            }
            Record record = command(i32(e.args[0]));
            for (size_t index = 1; index < e.args.size(); ++index)
            {
                record.operands.push_back(operand(e.args[index], index - 1));
            }
            rows.push_back(std::move(record));
            return;
        }
        if (e.name == "ui.meter.configure")
        {
            const auto args = arguments(e, {"segments"});
            const auto segments = json_value(args.at(0));
            if (!segments.is_array())
            {
                fail("meter segments must be an array");
            }
            Record record = command(181, {number(0), number(int32_t(checked32(segments.size())))});
            for (const auto& segment : segments)
            {
                if (!segment.is_object() || segment.size() != 2 || !segment.contains("percent") ||
                    !segment.contains("steps"))
                {
                    fail("each meter segment needs percent and steps");
                }
                for (const auto* key : {"percent", "steps"})
                {
                    const auto& value = segment.at(key);
                    if (!value.is_number_integer() || value.get<int64_t>() < INT32_MIN ||
                        value.get<int64_t>() > INT32_MAX)
                    {
                        fail("meter values must be signed 32-bit integers");
                    }
                    record.operands.push_back(number(value.get<int32_t>()));
                }
            }
            emit(record);
            return;
        }
        if (e.name == "ui.meter.signal")
        {
            const auto args = arguments(e, {"value"});
            const auto value = i32(args.at(0));
            if (value == 0)
            {
                fail("use ui.meter.configure for selector zero");
            }
            emit(command(181, {number(value)}));
            return;
        }
        if (e.name == "flow.jump" && e.args.size() == 1 && e.args[0].is_literal &&
            e.args[0].literal.is_string())
        {
            pending.push_back({rows.size(), 0, string(e.args[0])});
            emit(command(1, {number(0)}));
            return;
        }
        auto it = specs().names.find(e.name);
        Record r;
        std::vector<Expr> args;
        size_t start = 0;
        if (e.name == "native.opcode")
        {
            if (e.args.empty())
            {
                fail("missing opcode");
            }
            r.opcode = i32(e.args[0]);
            args.assign(e.args.begin() + 1, e.args.end());
        }
        else if (it != specs().names.end())
        {
            const auto& [obj, s] = it->second;
            const int c = s.code;
            auto params = s.params;
            if (obj)
            {
                const auto variant = specs().object_variants.find({c, e.args.size() - 1});
                if (!e.args.empty() && variant != specs().object_variants.end() &&
                    std::find(e.keys.begin(), e.keys.end(), "mode") == e.keys.end())
                {
                    params = variant->second.params;
                }
                params.insert(params.begin(), "group");
                if ((c == 5 || c == 7) && e.args.size() == 2)
                {
                    params.resize(2);
                }
            }
            args = arguments(e, params, obj && c == 1);
            for (size_t index = 0; index < args.size() && index < params.size(); ++index)
            {
                auto& argument = args[index];
                if (!argument.is_literal || !argument.literal.is_string())
                {
                    continue;
                }
                if (params[index] == "speaker")
                {
                    fail("speaker must be a numeric channel ID");
                }
                std::string role;
                if (params[index] == "sound")
                {
                    role = (c == 131 || c == 138 || c == 44)   ? "music"
                           : (c == 43 || c == 109 || c == 189) ? "voice"
                                                               : "sfx";
                }
                else if (obj && params[index] == "image")
                {
                    role = "image";
                }
                else if (params[index] == "transition")
                {
                    role = "transition";
                }
                else if (params[index] == "group" || params[index] == "scene" ||
                         params[index] == "script" || params[index] == "page")
                {
                    role = params[index];
                }
                else if (!obj && c == 79 && index == 0)
                {
                    role = "movie";
                }
                else if (!obj && c == 72 && index == 0)
                {
                    role = "video";
                }
                else if (!obj && (c == 32 || c == 112) && params[index] == "image")
                {
                    role = "resource";
                }
                if (!role.empty())
                {
                    if (!resource_resolver)
                    {
                        fail("resource paths require compilation through project.json");
                    }
                    argument.literal = resource_resolver(role,
                                                         role == "image" ? i32(args[0]) : -1,
                                                         argument.literal.get<std::string>());
                }
            }
            if (obj)
            {
                if (args.empty())
                {
                    fail("missing graphics group");
                }
                r.marker = '$';
                r.opcode = i32(args[0]);
                r.operands.push_back(number(c));
                start = 1;
            }
            else
            {
                r.opcode = c;
            }
        }
        else
        {
            fail("unknown instruction " + e.name);
        }
        for (size_t i = start; i < args.size(); i++)
        {
            r.operands.push_back(operand(args[i], r.operands.size()));
        }
        emit(r);
    }

    void body(unsigned depth)
    {
        while (!eat("}"))
        {
            if (is("<eof>"))
            {
                fail("unclosed scenario/block");
            }
            if (eat("if"))
            {
                need("(");
                auto left = value();
                Record condition;
                if (left.call && left.name == "expression" && left.args.size() == 1)
                {
                    condition =
                        command(139, {operand(left.args[0], 0), number(int32_t(depth + 1))});
                }
                else
                {
                    auto cmp = ts[at++].text;
                    static const std::map<std::string, int> ops = {
                        {"!=", 33}, {"<", 60}, {"==", 61}, {">", 62}, {"<=", 91}, {">=", 93}};
                    if (!ops.contains(cmp))
                    {
                        fail("invalid comparison");
                    }
                    auto right = value();
                    condition = command(8,
                                        {operand(left, 0),
                                         number(ops.at(cmp)),
                                         operand(right, 2),
                                         number(int32_t(depth + 1))});
                }
                need(")");
                need("{");
                emit(condition);
                body(depth + 1);
                if (eat("else"))
                {
                    emit(command(9, {number(int32_t(depth + 1))}));
                    need("{");
                    body(depth + 1);
                }
                emit(command(10, {number(int32_t(depth + 1))}));
                continue;
            }
            if (ts[at].text.starts_with("v.variable_") && ts[at + 1].text == "=")
            {
                auto dst = value();
                need("=");
                auto left = value();
                if (is("+") || is("-") || is("*") || is("/"))
                {
                    auto op = ts[at++].text;
                    auto right = value();
                    int code = op == "+" ? 4 : op == "-" ? 5 : op == "*" ? 6 : 7;
                    emit(command(code, {operand(dst, 0), operand(left, 1), operand(right, 2)}));
                }
                else
                {
                    emit(command(3, {operand(dst, 0), operand(left, 1)}));
                }
                need(";");
                continue;
            }
            auto e = value();
            need(";");
            call(e);
        }
    }

  public:
    explicit Parser(const std::string& s, ResourceResolver resolver = {})
        : ts(lex(s)), resource_resolver(std::move(resolver))
    {
    }

    PayloadMap parse()
    {
        PayloadMap out;
        bool format = false;
        while (!is("<eof>"))
        {
            if (eat("const"))
            {
                auto name = identifier();
                need("=");
                auto n = i32(value());
                need(";");
                if (transition_constants.contains(name) || !constants.emplace(name, n).second)
                {
                    fail("duplicate constant");
                }
                continue;
            }
            auto e = value();
            if (!e.call)
            {
                fail("expected top-level declaration");
            }
            if (e.name == "format")
            {
                if (format || e.args.size() != 1 || string(e.args[0]) != "csystem-script-1")
                {
                    fail("unsupported source format");
                }
                format = true;
                need(";");
                continue;
            }
            if (!format)
            {
                fail("missing format declaration");
            }
            if (e.args.empty())
            {
                fail("missing archive entry id");
            }
            auto id = u32(e.args[0]);
            if (out.contains(id))
            {
                fail("duplicate archive entry");
            }
            if (e.name == "scenario")
            {
                if (e.args.size() != 1)
                {
                    fail("scenario(id)");
                }
                scene = id;
                rows.clear();
                labels.clear();
                pending.clear();
                need("{");
                body(0);
                for (auto& p : pending)
                {
                    auto it = labels.find(p.label);
                    if (it == labels.end() || it->second >= rows.size())
                    {
                        fail("missing or out-of-range label " + p.label);
                    }
                    rows.at(p.row).operands.at(p.arg).value = int32_t(it->second);
                }
                std::vector<int> blocks;
                for (auto& r : rows)
                {
                    if (r.tag != 'M' || r.marker != '#')
                    {
                        continue;
                    }
                    if (r.opcode == 8 || r.opcode == 139)
                    {
                        blocks.push_back(r.operands.back().value);
                    }
                    if (r.opcode == 9 || r.opcode == 10)
                    {
                        if (blocks.empty() || blocks.back() != r.operands.at(0).value)
                        {
                            fail("unmatched condition marker");
                        }
                        if (r.opcode == 10)
                        {
                            blocks.pop_back();
                        }
                    }
                }
                if (!blocks.empty())
                {
                    fail("unclosed condition markers");
                }
                out[id] = encode_records(rows);
                continue;
            }
            if (e.args.size() != 2)
            {
                fail("data declaration requires id and object");
            }
            auto j = json_value(e.args[1]);
            if (e.name == "javascript")
            {
                JavaScript js;
                js.id = j.at("id");
                js.name = utf16(j.at("name"));
                js.source = utf16(j.at("source"));
                js.dependencies = j.at("dependencies").get<std::vector<uint32_t>>();
                out[id] = encode_javascript(js);
            }
            else if (e.name == "ui")
            {
                Metadata m;
                m.name = utf16(j.at("name"));
                for (auto& p : j.at("properties"))
                {
                    Property prop;
                    prop.name = utf16(p.at("name"));
                    for (auto& v : p.at("values"))
                    {
                        MetadataValue mv;
                        mv.type = v.at("type");
                        if (mv.type == 1)
                        {
                            mv.text = utf16(v.at("value"));
                        }
                        else if (mv.type == 2)
                        {
                            mv.number = v.at("value");
                        }
                        else if (mv.type == 3)
                        {
                            mv.id = v.at("value");
                        }
                        else
                        {
                            fail("unknown UI value type");
                        }
                        prop.values.push_back(mv);
                    }
                    m.properties.push_back(prop);
                }
                out[id] = encode_metadata(m);
            }
            else if (e.name == "sound_group")
            {
                SoundGroup g;
                g.name = utf16(j.at("name"));
                g.ids = j.at("sounds").get<std::vector<uint32_t>>();
                out[id] = encode_sound_group(g);
            }
            else
            {
                fail("unknown declaration " + e.name);
            }
            need(";");
        }
        if (!format)
        {
            fail("missing source format");
        }
        return out;
    }
};
}

PayloadMap compile_script(const std::string& s, const ResourceResolver& resolver)
{
    return Parser(s, resolver).parse();
}

std::string decompile_script(const std::vector<Entry>& entries,
                             const ResourcePathLookup& paths,
                             const ResourceResolver& resolver)
{
    std::ostringstream body;
    std::map<std::string, int32_t> constants;
    auto constant = [&](const std::string& prefix, int32_t n)
    {
        if (n < 0)
        {
            return std::to_string(n);
        }
        auto name = prefix + "_" + padded(uint32_t(n));
        constants[name] = n;
        return name;
    };
    for (auto& e : entries)
    {
        if (e.subtype != '0')
        {
            continue;
        }
        if (e.kind == 'a')
        {
            auto rows = decode_records(e.payload);
            auto tg = targets(rows);
            std::map<size_t, std::string> labels;
            for (size_t i = 0; i < rows.size(); i++)
            {
                if (rows[i].typed && rows[i].tag == 'M' && rows[i].marker == 'L' &&
                    rows[i].operands.empty())
                {
                    labels[i] = "label_" + padded(i, 5);
                }
            }
            for (auto& [i, ps] : tg)
            {
                for (auto p : ps)
                {
                    if (p < rows[i].operands.size())
                    {
                        auto& o = rows[i].operands[p];
                        if (o.type == 'N' && o.value >= 0 && size_t(o.value) < rows.size())
                        {
                            labels.try_emplace(size_t(o.value), "position_" + padded(o.value, 5));
                        }
                    }
                }
            }
            body << "scenario(" << e.id << ") {\n";
            unsigned depth = 1;
            auto emit = [&](const std::string& s)
            {
                body << std::string(depth * 4, ' ') << s << "\n";
            };
            auto operand_text = [&](const Operand& o,
                                    size_t index,
                                    const std::string& param,
                                    int code,
                                    int group) -> std::string
            {
                if (o.type == 'A')
                {
                    if (o.value < 0)
                    {
                        return "var(" + std::to_string(o.value) + ")";
                    }
                    return "v.variable_" + padded(uint32_t(o.value));
                }
                if (o.type == 'T')
                {
                    if (o.text_id == uint64_t(e.id) * 100000 + index + 1)
                    {
                        return quote(utf8(o.text));
                    }
                    return "text.literal(" + std::to_string(o.text_id) + ", " +
                           quote(utf8(o.text)) + ")";
                }
                if (param == "speaker")
                {
                    return std::to_string(o.value);
                }
                if (param == "scene" || param == "script" || param == "group" || param == "page")
                {
                    const auto path = paths ? paths(param, -1, o.value) : "";
                    return path.empty() ? constant(param, o.value) : quote(path);
                }
                if (param == "transition")
                {
                    for (const auto& [name, value] : transition_constants)
                    {
                        if (o.value == value)
                        {
                            return name;
                        }
                    }
                    const auto path = paths ? paths("transition", -1, o.value) : "";
                    return path.empty() ? constant("transition", o.value) : quote(path);
                }
                if (param == "movie" || param == "timeline")
                {
                    const std::string role = code == 79 ? "movie" : "video";
                    const auto path = paths ? paths(role, -1, o.value) : "";
                    return path.empty() ? constant(role, o.value) : quote(path);
                }
                if (param == "sound")
                {
                    const std::string role = (code == 131 || code == 138 || code == 44)   ? "music"
                                             : (code == 43 || code == 109 || code == 189) ? "voice"
                                                                                          : "sfx";
                    const auto path = paths ? paths(role, -1, o.value) : "";
                    return path.empty() ? constant(role, o.value) : quote(path);
                }
                if (param == "image" && group >= 0)
                {
                    const auto path = paths ? paths("image", group, o.value) : "";
                    return path.empty() ? constant("image_" + padded(group), o.value) : quote(path);
                }
                if (param == "image" && (code == 32 || code == 112))
                {
                    const auto path = paths ? paths("resource", -1, o.value) : "";
                    return path.empty() ? std::to_string(o.value) : quote(path);
                }
                return std::to_string(o.value);
            };
            for (size_t i = 0; i < rows.size(); i++)
            {
                auto& r = rows[i];
                bool label = r.typed && r.tag == 'M' && r.marker == 'L' && r.operands.empty() &&
                             r.trailer == 9999;
                if (labels.contains(i))
                {
                    emit(std::string(label ? "flow.label(" : "flow.mark(") + quote(labels[i]) +
                         (label ? ", " + std::to_string(r.opcode) : "") + ");");
                }
                if (label)
                {
                    continue;
                }
                if (!r.typed || r.trailer != 9999)
                {
                    auto b = encode_records({r});
                    emit("binary.record(" + quote(hex(std::span(b).subspan(4))) + ");");
                    continue;
                }
                if (r.tag == 'T')
                {
                    std::string s = "dialogue.say(" + quote(utf8(r.text));
                    if (r.text_id != uint64_t(e.id) * 100000 + i + 1)
                    {
                        s += ", " + std::to_string(r.text_id);
                    }
                    emit(s + ");");
                    continue;
                }
                try
                {
                    validate(r);
                }
                catch (const Error&)
                {
                    if (r.marker != '#')
                    {
                        const auto bytes = encode_records({r});
                        emit("binary.record(" + quote(hex(std::span(bytes).subspan(4))) + ");");
                        continue;
                    }
                    std::string line = "native.command(" + std::to_string(r.opcode);
                    for (const auto& operand : r.operands)
                    {
                        line += ", " + operand_text(operand, i, "", r.opcode, -1);
                    }
                    emit(line + ");");
                    continue;
                }
                if (r.marker == '#' && (r.opcode == 8 || r.opcode == 139) &&
                    r.operands.back().value == int(depth))
                {
                    validate(r);
                    std::string cond;
                    if (r.opcode == 139)
                    {
                        cond =
                            "expression(" + operand_text(r.operands[0], i, "", r.opcode, -1) + ")";
                    }
                    else
                    {
                        static const std::map<int, std::string> ops = {
                            {33, "!="}, {60, "<"}, {61, "=="}, {62, ">"}, {91, "<="}, {93, ">="}};
                        if (!ops.contains(r.operands[1].value))
                        {
                            throw Error("unknown condition comparator");
                        }
                        cond = operand_text(r.operands[0], i, "", 8, -1) + " " +
                               ops.at(r.operands[1].value) + " " +
                               operand_text(r.operands[2], i, "", 8, -1);
                    }
                    emit("if (" + cond + ") {");
                    depth++;
                    continue;
                }
                if (r.marker == '#' && (r.opcode == 9 || r.opcode == 10) && depth > 1 &&
                    r.operands.at(0).value == int(depth - 1))
                {
                    depth--;
                    emit(r.opcode == 9 ? "} else {" : "}");
                    if (r.opcode == 9)
                    {
                        depth++;
                    }
                    continue;
                }
                if (r.marker == '#' && r.opcode >= 3 && r.opcode <= 7 && !r.operands.empty() &&
                    r.operands[0].value >= 0)
                {
                    validate(r);
                    auto& a = r.operands;
                    auto s = operand_text(a[0], i, "", r.opcode, -1) + " = " +
                             operand_text(a[1], i, "", r.opcode, -1);
                    if (r.opcode != 3)
                    {
                        s += " " +
                             std::string(r.opcode == 4   ? "+"
                                         : r.opcode == 5 ? "-"
                                         : r.opcode == 6 ? "*"
                                                         : "/") +
                             " " + operand_text(a[2], i, "", r.opcode, -1);
                    }
                    emit(s + ";");
                    continue;
                }
                bool obj = r.marker == '$';
                int c = obj && !r.operands.empty() ? r.operands[0].value : r.opcode;
                auto& table = obj ? specs().objects : specs().commands;
                auto it = table.find(c);
                if ((r.marker != '#' && !obj) || it == table.end())
                {
                    if (r.marker == '#')
                    {
                        std::string s = "native.opcode(" + std::to_string(c);
                        for (auto& o : r.operands)
                        {
                            s += ", " + operand_text(o, i, "", c, -1);
                        }
                        emit(s + ");");
                    }
                    else
                    {
                        auto b = encode_records({r});
                        emit("binary.record(" + quote(hex(std::span(b).subspan(4))) + ");");
                    }
                    continue;
                }
                validate(r);
                if (!obj && c == 181)
                {
                    if (r.operands[0].value != 0)
                    {
                        emit("ui.meter.signal(value=" + std::to_string(r.operands[0].value) + ");");
                    }
                    else
                    {
                        emit("ui.meter.configure(segments=[");
                        ++depth;
                        for (size_t item = 2; item < r.operands.size(); item += 2)
                        {
                            emit("{\"percent\": " + std::to_string(r.operands[item].value) +
                                 ", \"steps\": " + std::to_string(r.operands[item + 1].value) +
                                 "}" + (item + 2 < r.operands.size() ? "," : ""));
                        }
                        --depth;
                        emit("]);");
                    }
                    continue;
                }
                const auto& variants = obj ? specs().object_variants : specs().variants;
                const auto variant = variants.find({c, r.operands.size() - (obj ? 1 : 0)});
                const auto& s = variant != variants.end() ? variant->second : it->second;
                std::string line = s.name + "(";
                bool any = false;
                if (obj)
                {
                    line += "group=" + operand_text(number(r.opcode), i, "group", c, -1);
                    any = true;
                }
                for (size_t p = obj ? 1 : 0; p < r.operands.size(); p++)
                {
                    if (any)
                    {
                        line += ", ";
                    }
                    any = true;
                    size_t k = p - (obj ? 1 : 0);
                    std::string param = k < s.params.size() ? s.params[k] : "";
                    bool named = r.operands.size() <= s.params.size() + (obj ? 1 : 0) && c != 181;
                    if (named)
                    {
                        line += param + "=";
                    }
                    bool target = !obj && tg.contains(i) &&
                                  std::find(tg[i].begin(), tg[i].end(), p) != tg[i].end();
                    auto& o = r.operands[p];
                    if (target && o.type == 'N' && o.value >= 0 && labels.contains(size_t(o.value)))
                    {
                        line += "ref(" + quote(labels[size_t(o.value)]) + ")";
                    }
                    else
                    {
                        line += operand_text(o, i, param, c, obj ? r.opcode : -1);
                    }
                }
                emit(line + ");");
            }
            if (depth != 1)
            {
                throw Error("unbalanced source control flow");
            }
            body << "}\n\n";
        }
        else if (e.kind == 's')
        {
            auto js = decode_javascript(e.payload);
            Json j = {{"id", js.id},
                      {"name", utf8(js.name)},
                      {"source", utf8(js.source)},
                      {"dependencies", js.dependencies}};
            body << "javascript(" << e.id << ", " << j.dump(2) << ");\n\n";
        }
        else if (e.kind == 'B')
        {
            auto m = decode_metadata(e.payload);
            Json j = {{"name", utf8(m.name)}, {"properties", Json::array()}};
            for (auto& p : m.properties)
            {
                Json values = Json::array();
                for (auto& v : p.values)
                {
                    Json value;
                    if (v.type == 1)
                    {
                        value = utf8(v.text);
                    }
                    else if (v.type == 2)
                    {
                        value = v.number;
                    }
                    else
                    {
                        value = v.id;
                    }
                    values.push_back({{"type", v.type}, {"value", value}});
                }
                j["properties"].push_back({{"name", utf8(p.name)}, {"values", values}});
            }
            body << "ui(" << e.id << ", " << j.dump(2) << ");\n\n";
        }
        else if (e.kind == 'C')
        {
            auto g = decode_sound_group(e.payload);
            body << "sound_group(" << e.id << ", "
                 << Json({{"name", utf8(g.name)}, {"sounds", g.ids}}).dump(2) << ");\n\n";
        }
        else if (e.kind == 'f')
        {
            throw Error("graphics groups use JSON documents");
        }
    }
    std::ostringstream out;
    out << "format(\"csystem-script-1\");\n\n";
    for (auto& [n, v] : constants)
    {
        out << "const " << n << " = " << v << ";\n";
    }
    if (!constants.empty())
    {
        out << "\n";
    }
    out << body.str();
    auto source = out.str();
    auto rebuilt = compile_script(source, resolver);
    for (auto& e : entries)
    {
        if (rebuilt.contains(e.id) && rebuilt.at(e.id) != e.payload)
        {
            throw Error("source round-trip mismatch at entry " + std::to_string(e.id));
        }
    }
    return source;
}
}
