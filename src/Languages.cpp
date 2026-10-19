#include "Languages.h"

#include <algorithm>
#include <cctype>

namespace {

std::unordered_set<std::string> words(const char* s)
{
    std::unordered_set<std::string> out;
    std::string cur;
    for (const char* p = s;; ++p)
    {
        if (*p == ' ' || *p == '\n' || *p == 0)
        {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
            if (*p == 0) break;
        }
        else
        {
            cur.push_back(*p);
        }
    }
    return out;
}

// Shared word lists.
const char* kCKeywords =
    "auto break case const continue default do else enum extern for goto if inline register restrict return "
    "sizeof static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn "
    "_Static_assert _Thread_local alignas alignof static_assert thread_local typeof constexpr";
const char* kCTypes =
    "void char short int long float double signed unsigned bool _Bool size_t ssize_t ptrdiff_t intptr_t "
    "uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t FILE wchar_t char16_t char32_t";
const char* kCppKeywords =
    "alignas alignof and asm auto break case catch class co_await co_return co_yield concept const const_cast "
    "consteval constexpr constinit continue decltype default delete do dynamic_cast else enum explicit export "
    "extern final for friend goto if import inline module mutable namespace new noexcept not operator or "
    "override private protected public register reinterpret_cast requires return sizeof static static_assert "
    "static_cast struct switch template this thread_local throw try typedef typeid typename union using virtual "
    "volatile while xor";
const char* kCppTypes =
    "void char char8_t char16_t char32_t wchar_t short int long float double signed unsigned bool size_t "
    "ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
    "std string string_view vector map unordered_map set unordered_set array optional variant unique_ptr "
    "shared_ptr weak_ptr span FILE";
const char* kCConstants = "true false nullptr NULL EOF stdin stdout stderr";

Language& add(std::vector<Language>& v, const char* id, const char* name)
{
    v.emplace_back();
    v.back().id = id;
    v.back().name = name;
    return v.back();
}

void cStyle(Language& l)
{
    l.lineComment = "//";
    l.blockStart = "/*";
    l.blockEnd = "*/";
}

std::vector<Language> build()
{
    std::vector<Language> v;
    v.reserve(64);

    // --- C family --------------------------------------------------------------------
    {
        Language& l = add(v, "c", "C");
        l.extensions = { "c", "h" };
        cStyle(l);
        l.preprocessor = true;
        l.keywords = words(kCKeywords);
        l.types = words(kCTypes);
        l.constants = words(kCConstants);
        l.build = "cc -std=c17 -g -O0 -Wall -Wextra {file} -o {out}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "cpp", "C++");
        l.extensions = { "cpp", "cc", "cxx", "c++", "hpp", "hh", "hxx", "h++", "inl", "ipp", "tpp", "ixx", "cppm" };
        cStyle(l);
        l.preprocessor = true;
        l.keywords = words(kCppKeywords);
        l.types = words(kCppTypes);
        l.constants = words(kCConstants);
        l.build = "c++ -std=c++20 -g -O0 -Wall -Wextra {file} -o {out}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "objc", "Objective-C");
        l.extensions = { "m", "mm" };
        cStyle(l);
        l.preprocessor = true;
        l.decorators = true;
        l.keywords = words((std::string(kCppKeywords) + " interface implementation end property synthesize "
                                                        "protocol selector autoreleasepool nonatomic strong weak "
                                                        "copy assign readonly readwrite self super id instancetype")
                               .c_str());
        l.types = words((std::string(kCTypes) + " BOOL NSString NSObject NSArray NSDictionary NSInteger NSUInteger "
                                                "CGFloat")
                            .c_str());
        l.constants = words("YES NO nil Nil NULL true false");
        l.build = "clang -fobjc-arc -g -O0 -Wall {file} -framework Foundation -o {out}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "csharp", "C#");
        l.extensions = { "cs", "csx" };
        cStyle(l);
        l.preprocessor = true;
        l.decorators = true;
        l.keywords = words(
            "abstract as base break case catch checked class const continue default delegate do else enum event "
            "explicit extern finally fixed for foreach goto if implicit in interface internal is lock namespace "
            "new operator out override params private protected public readonly record ref return sealed sizeof "
            "stackalloc static struct switch this throw try typeof unchecked unsafe using virtual volatile while "
            "async await var get set init yield where when with required file global partial");
        l.types = words("bool byte sbyte char decimal double float int uint long ulong short ushort object string "
                        "void dynamic nint nuint List Dictionary Task Span");
        l.constants = words("true false null");
        l.run = "dotnet run {file}";
    }
    {
        Language& l = add(v, "java", "Java");
        l.extensions = { "java" };
        cStyle(l);
        l.decorators = true;
        l.keywords = words(
            "abstract assert break case catch class const continue default do else enum extends final finally for "
            "goto if implements import instanceof interface native new package private protected public return "
            "static strictfp super switch synchronized this throw throws transient try volatile while var record "
            "sealed permits yield non-sealed module requires exports");
        l.types = words("boolean byte char double float int long short void String Object Integer Long Double "
                        "List Map Set ArrayList HashMap Optional");
        l.constants = words("true false null");
        l.build = "mkdir -p {out} && javac -g -d {out} {file}";
        l.run = "java {file}";
    }
    {
        Language& l = add(v, "kotlin", "Kotlin");
        l.extensions = { "kt", "kts" };
        cStyle(l);
        l.tripleQuotes = true;
        l.decorators = true;
        l.keywords = words(
            "as break class continue do else for fun if in interface is null object package return super this "
            "throw try typealias typeof val var when while by catch constructor delegate dynamic field file finally "
            "get import init param property receiver set setparam where actual abstract annotation companion const "
            "crossinline data enum expect external final infix inline inner internal lateinit noinline open "
            "operator out override private protected public reified sealed suspend tailrec vararg");
        l.types = words("Int Long Short Byte Double Float Boolean Char String Unit Any Nothing List Map Set Array");
        l.constants = words("true false null");
        l.build = "kotlinc {file} -include-runtime -d {out}.jar";
        l.run = "java -jar {out}.jar";
        l.compiled = true;
    }
    {
        Language& l = add(v, "swift", "Swift");
        l.extensions = { "swift" };
        cStyle(l);
        l.tripleQuotes = true;
        l.decorators = true;
        l.keywords = words(
            "associatedtype class deinit enum extension fileprivate func import init inout internal let open "
            "operator private precedencegroup protocol public rethrows static struct subscript typealias var break "
            "case catch continue default defer do else fallthrough for guard if in repeat return throw switch where "
            "while as is try throws async await actor some any self Self super convenience dynamic final lazy "
            "mutating nonmutating optional override required unowned weak willSet didSet get set");
        l.types = words("Int Int8 Int16 Int32 Int64 UInt Double Float Bool String Character Array Dictionary Set "
                        "Optional Void Any AnyObject");
        l.constants = words("true false nil");
        l.build = "swiftc -g -Onone {file} -o {out}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "go", "Go");
        l.extensions = { "go" };
        cStyle(l);
        l.backtickStrings = true;
        l.useTabs = true;
        l.keywords = words("break case chan const continue default defer else fallthrough for func go goto if "
                           "import interface map package range return select struct switch type var");
        l.types = words("bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune "
                        "string uint uint8 uint16 uint32 uint64 uintptr any comparable");
        l.constants = words("true false nil iota");
        l.build = "go build -gcflags=all='-N -l' -o {out} {file}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "rust", "Rust");
        l.extensions = { "rs" };
        cStyle(l);
        l.shortCharLiterals = true;
        l.stringPrefixes = "rb";
        l.decorators = false;
        l.keywords = words("as async await break const continue crate dyn else enum extern fn for if impl in let "
                           "loop match mod move mut pub ref return self Self static struct super trait type unsafe "
                           "use where while macro_rules union");
        l.types = words("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec "
                        "Option Result Box Rc Arc HashMap HashSet");
        l.constants = words("true false None Some Ok Err");
        l.build = "rustc -g --edition 2021 {file} -o {out}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "zig", "Zig");
        l.extensions = { "zig" };
        l.lineComment = "//";
        l.keywords = words("addrspace align allowzero and anyframe anytype asm async await break callconv catch "
                           "comptime const continue defer else enum errdefer error export extern fn for if inline "
                           "noalias nosuspend noinline opaque or orelse packed pub resume return linksection "
                           "struct suspend switch test threadlocal try union unreachable usingnamespace var "
                           "volatile while");
        l.types = words("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f16 f32 f64 f128 bool void type "
                        "anyerror noreturn comptime_int comptime_float");
        l.constants = words("true false null undefined");
        l.build = "zig build-exe -femit-bin={out} {file}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "d", "D");
        l.extensions = { "d" };
        cStyle(l);
        l.keywords = words("abstract alias align asm assert auto body break case cast catch class const continue "
                           "debug default delegate deprecated do else enum export extern final finally for foreach "
                           "function goto if immutable import in inout interface invariant is lazy mixin module "
                           "new nothrow out override package pragma private protected public pure ref return scope "
                           "shared static struct super switch synchronized template this throw try typeid typeof "
                           "union unittest version while with");
        l.types = words("bool byte ubyte short ushort int uint long ulong float double real char wchar dchar "
                        "string void size_t");
        l.constants = words("true false null");
        l.run = "rdmd {file}";
    }
    {
        Language& l = add(v, "dart", "Dart");
        l.extensions = { "dart" };
        cStyle(l);
        l.tripleQuotes = true;
        l.decorators = true;
        l.keywords = words("abstract as assert async await break case catch class const continue covariant "
                           "default deferred do dynamic else enum export extends extension external factory final "
                           "finally for get hide if implements import in interface is late library mixin new "
                           "on operator part required rethrow return set show static super switch sync this "
                           "throw try typedef var while with yield");
        l.types = words("int double num bool String List Map Set void Future Stream Object dynamic");
        l.constants = words("true false null");
        l.build = "dart analyze {file}";
        l.run = "dart run {file}";
    }
    {
        Language& l = add(v, "scala", "Scala");
        l.extensions = { "scala", "sc", "sbt" };
        cStyle(l);
        l.tripleQuotes = true;
        l.decorators = true;
        l.keywords = words("abstract case catch class def do else enum export extends final finally for given "
                           "if implicit import lazy match new object override package private protected return "
                           "sealed super then this throw trait try type using val var while with yield");
        l.types = words("Int Long Double Float Boolean Char String Unit Any Nothing List Map Set Option Seq");
        l.constants = words("true false null None Some");
        l.run = "scala run {file}";
    }
    {
        Language& l = add(v, "groovy", "Groovy");
        l.extensions = { "groovy", "gradle" };
        cStyle(l);
        l.tripleQuotes = true;
        l.decorators = true;
        l.keywords = words("as assert break case catch class def default do else enum extends final finally for "
                           "if implements import in instanceof interface new package return static super switch "
                           "this throw throws trait try var while");
        l.constants = words("true false null");
        l.run = "groovy {file}";
    }
    {
        Language& l = add(v, "javascript", "JavaScript");
        l.extensions = { "js", "mjs", "cjs", "jsx" };
        l.shebangs = { "node" };
        cStyle(l);
        l.backtickStrings = true;
        l.decorators = true;
        l.keywords = words("async await break case catch class const continue debugger default delete do else "
                           "export extends finally for from function get if import in instanceof let new of "
                           "return set static super switch this throw try typeof var void while with yield");
        l.types = words("Array Object String Number Boolean Map Set Promise Date RegExp Error JSON Math console "
                        "window document");
        l.constants = words("true false null undefined NaN Infinity");
        l.build = "node --check {file}";
        l.run = "node {file}";
    }
    {
        Language& l = add(v, "typescript", "TypeScript");
        l.extensions = { "ts", "tsx", "mts", "cts" };
        l.shebangs = { "ts-node", "tsx", "deno", "bun" };
        cStyle(l);
        l.backtickStrings = true;
        l.decorators = true;
        l.keywords = words("abstract as async await break case catch class const continue declare default delete "
                           "do else enum export extends finally for from function get if implements import in "
                           "infer instanceof interface is keyof let namespace new of private protected public "
                           "readonly return satisfies set static super switch this throw try type typeof var void "
                           "while yield");
        l.types = words("any boolean never number object string symbol unknown void bigint Array Record Partial "
                        "Promise Map Set Date Error");
        l.constants = words("true false null undefined NaN Infinity");
        // Whichever TypeScript runtime is installed; npx fetches tsx as a last resort.
        l.run = "if command -v bun >/dev/null; then bun {file}; elif command -v deno >/dev/null; then deno run -A {file}; "
                "else npx --yes tsx {file}; fi";
    }
    {
        Language& l = add(v, "php", "PHP");
        l.extensions = { "php", "phtml" };
        l.shebangs = { "php" };
        cStyle(l);
        l.lineComment2 = "#";
        l.dollarVariables = true;
        l.caseInsensitive = true;
        l.keywords = words("abstract and as break callable case catch class clone const continue declare default "
                           "do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum "
                           "extends final finally fn for foreach function global goto if implements include "
                           "include_once instanceof insteadof interface isset list match namespace new or print "
                           "private protected public readonly require require_once return static switch throw "
                           "trait try unset use var while xor yield");
        l.types = words("int float string bool array object mixed void iterable self parent");
        l.constants = words("true false null");
        l.build = "php -l {file}";
        l.run = "php {file}";
    }
    {
        Language& l = add(v, "css", "CSS");
        l.extensions = { "css", "scss", "sass", "less" };
        l.blockStart = "/*";
        l.blockEnd = "*/";
        l.lineComment = "//";  // SCSS / Less
        l.keywords = words("@media @import @font-face @keyframes @supports @mixin @include @extend @use @forward "
                           "@if @else @each @for @function @return !important");
        l.constants = words("none auto inherit initial unset block inline flex grid absolute relative fixed "
                            "sticky hidden visible solid");
    }
    {
        Language& l = add(v, "glsl", "Shader");
        l.extensions = { "glsl", "vert", "frag", "geom", "comp", "tesc", "tese", "hlsl", "fx", "metal", "wgsl",
                         "cu", "cuh" };
        cStyle(l);
        l.preprocessor = true;
        l.keywords = words("attribute const uniform varying buffer shared layout centroid flat smooth break "
                           "continue do for while switch case default if else in out inout return discard struct "
                           "kernel vertex fragment device constant thread threadgroup using namespace fn let var "
                           "__global__ __device__ __host__ __shared__");
        l.types = words("float double int uint bool void vec2 vec3 vec4 ivec2 ivec3 ivec4 mat2 mat3 mat4 "
                        "sampler2D float2 float3 float4 half half2 half3 half4 float4x4 texture2d f32 i32 u32");
        l.constants = words("true false");
    }
    {
        Language& l = add(v, "solidity", "Solidity");
        l.extensions = { "sol" };
        cStyle(l);
        l.keywords = words("pragma solidity contract interface library function modifier event struct enum "
                           "mapping public private internal external pure view payable returns return if else for "
                           "while do break continue emit import using is memory storage calldata require revert "
                           "constructor override virtual");
        l.types = words("address bool string bytes uint int uint256 int256 bytes32 uint8");
        l.constants = words("true false");
    }
    {
        Language& l = add(v, "protobuf", "Protocol Buffers");
        l.extensions = { "proto" };
        cStyle(l);
        l.keywords = words("syntax package import option message enum service rpc returns repeated optional "
                           "required oneof map reserved extend stream");
        l.types = words("double float int32 int64 uint32 uint64 sint32 sint64 fixed32 fixed64 bool string bytes");
        l.constants = words("true false");
    }

    // --- Scripting languages ------------------------------------------------------------
    {
        Language& l = add(v, "python", "Python");
        l.extensions = { "py", "pyw", "pyi" };
        l.shebangs = { "python", "python3", "python2" };
        l.lineComment = "#";
        l.tripleQuotes = true;
        l.stringPrefixes = "rbfuRBFU";
        l.decorators = true;
        l.indentAfterColon = true;
        l.braces = false;
        l.keywords = words("and as assert async await break class continue def del elif else except finally for "
                           "from global if import in is lambda nonlocal not or pass raise return try while with "
                           "yield match case type");
        l.types = words("int float str bool bytes list dict set tuple frozenset object type complex bytearray "
                        "range self cls");
        l.constants = words("True False None NotImplemented Ellipsis __name__ __file__");
        l.build = "python3 -m py_compile {file}";
        l.run = "python3 -u {file}";
        l.debugger = Language::Debugger::Pdb;
    }
    {
        Language& l = add(v, "ruby", "Ruby");
        l.extensions = { "rb", "rake", "gemspec", "ru" };
        l.filenames = { "Gemfile", "Rakefile" };
        l.shebangs = { "ruby" };
        l.lineComment = "#";
        l.blockStart = "=begin";
        l.blockEnd = "=end";
        l.braces = false;
        l.keywords = words("alias and begin break case class def defined? do else elsif end ensure for if in "
                           "module next not or redo rescue retry return self super then undef unless until when "
                           "while yield require require_relative attr_accessor attr_reader attr_writer private "
                           "protected public lambda proc puts");
        l.constants = words("true false nil __FILE__ __LINE__");
        l.build = "ruby -c {file}";
        l.run = "ruby {file}";
    }
    {
        Language& l = add(v, "perl", "Perl");
        l.extensions = { "pl", "pm", "t" };
        l.shebangs = { "perl" };
        l.lineComment = "#";
        l.dollarVariables = true;
        l.keywords = words("my our local sub if elsif else unless while until for foreach do last next redo "
                           "return use no package require print printf say die warn eval qw");
        l.build = "perl -c {file}";
        l.run = "perl {file}";
    }
    {
        Language& l = add(v, "lua", "Lua");
        l.extensions = { "lua" };
        l.shebangs = { "lua", "luajit" };
        l.lineComment = "--";
        l.blockStart = "--[[";
        l.blockEnd = "]]";
        l.braces = false;
        l.keywords = words("and break do else elseif end for function goto if in local not or repeat return then "
                           "until while");
        l.constants = words("true false nil");
        l.build = "luac -p {file}";
        l.run = "lua {file}";
    }
    {
        Language& l = add(v, "shell", "Shell");
        l.extensions = { "sh", "bash", "zsh", "ksh", "fish", "command" };
        l.filenames = { ".bashrc", ".zshrc", ".profile", ".bash_profile", ".zprofile" };
        l.shebangs = { "sh", "bash", "zsh", "ksh", "dash", "fish" };
        l.lineComment = "#";
        l.dollarVariables = true;
        l.braces = false;
        l.keywords = words("if then else elif fi case esac for while until do done in function select return "
                           "local export readonly declare set unset shift exit break continue source alias echo "
                           "printf cd test eval exec trap");
        l.constants = words("true false");
        l.build = "bash -n {file}";
        l.run = "bash {file}";
    }
    {
        Language& l = add(v, "powershell", "PowerShell");
        l.extensions = { "ps1", "psm1", "psd1" };
        l.shebangs = { "pwsh" };
        l.lineComment = "#";
        l.blockStart = "<#";
        l.blockEnd = "#>";
        l.dollarVariables = true;
        l.caseInsensitive = true;
        l.keywords = words("begin break catch class continue data do dynamicparam else elseif end exit filter "
                           "finally for foreach from function if in param process return switch throw trap try "
                           "until using while");
        l.constants = words("$true $false $null");
        l.run = "pwsh -File {file}";
    }
    {
        Language& l = add(v, "r", "R");
        l.extensions = { "r", "rmd" };
        l.shebangs = { "Rscript" };
        l.lineComment = "#";
        l.keywords = words("if else repeat while function for in next break return library require");
        l.constants = words("TRUE FALSE NULL NA NaN Inf T F");
        l.run = "Rscript {file}";
    }
    {
        Language& l = add(v, "julia", "Julia");
        l.extensions = { "jl" };
        l.shebangs = { "julia" };
        l.lineComment = "#";
        l.blockStart = "#=";
        l.blockEnd = "=#";
        l.tripleQuotes = true;
        l.braces = false;
        l.keywords = words("baremodule begin break catch const continue do else elseif end export finally for "
                           "function global if import let local macro module quote return struct try using while "
                           "abstract mutable primitive type where in isa");
        l.types = words("Int Int8 Int16 Int32 Int64 UInt Float32 Float64 Bool String Char Vector Matrix Array "
                        "Dict Any Nothing");
        l.constants = words("true false nothing missing Inf NaN");
        l.run = "julia {file}";
    }
    {
        Language& l = add(v, "elixir", "Elixir");
        l.extensions = { "ex", "exs" };
        l.shebangs = { "elixir" };
        l.lineComment = "#";
        l.tripleQuotes = true;
        l.decorators = true;
        l.braces = false;
        l.keywords = words("after alias and case catch cond def defp defmodule defmacro defstruct defprotocol "
                           "defimpl do else end fn for if import in not or quote raise receive require rescue "
                           "try unless unquote use when with");
        l.constants = words("true false nil");
        l.run = "elixir {file}";
    }
    {
        Language& l = add(v, "erlang", "Erlang");
        l.extensions = { "erl", "hrl" };
        l.lineComment = "%";
        l.singleQuoteStrings = false;
        l.keywords = words("after and andalso band begin bnot bor bsl bsr bxor case catch cond div end fun if "
                           "let not of or orelse receive rem try when xor module export import record define");
        l.constants = words("true false undefined ok error");
        l.run = "escript {file}";
    }
    {
        Language& l = add(v, "haskell", "Haskell");
        l.extensions = { "hs", "lhs" };
        l.shebangs = { "runghc", "runhaskell" };
        l.lineComment = "--";
        l.blockStart = "{-";
        l.blockEnd = "-}";
        l.shortCharLiterals = true;
        l.braces = false;
        l.keywords = words("case class data default deriving do else foreign if import in infix infixl infixr "
                           "instance let module newtype of then type where qualified as hiding");
        l.types = words("Int Integer Float Double Bool Char String Maybe Either IO");
        l.constants = words("True False Nothing Just Left Right");
        l.build = "mkdir -p $(dirname {out}) && ghc -g -outputdir $(dirname {out}) -o {out} {file}";
        l.run = "{out}";
        l.compiled = true;
    }
    {
        Language& l = add(v, "ocaml", "OCaml");
        l.extensions = { "ml", "mli" };
        l.blockStart = "(*";
        l.blockEnd = "*)";
        l.shortCharLiterals = true;
        l.braces = false;
        l.keywords = words("and as assert begin class constraint do done downto else end exception external for "
                           "fun function functor if in include inherit initializer lazy let match method module "
                           "mutable new object of open or private rec sig struct then to try type val virtual "
                           "when while with");
        l.types = words("int float bool char string unit list array option ref");
        l.constants = words("true false None Some");
        l.run = "ocaml {file}";
    }
    {
        Language& l = add(v, "fsharp", "F#");
        l.extensions = { "fs", "fsi", "fsx" };
        l.lineComment = "//";
        l.blockStart = "(*";
        l.blockEnd = "*)";
        l.tripleQuotes = true;
        l.shortCharLiterals = true;
        l.braces = false;
        l.keywords = words("abstract and as assert base begin class default delegate do done downcast downto elif "
                           "else end exception extern finally for fun function global if in inherit inline "
                           "interface internal lazy let match member module mutable namespace new not of open or "
                           "override private public rec return static struct then to try type upcast use val void "
                           "when while with yield");
        l.types = words("int float bool char string unit list array option seq");
        l.constants = words("true false null None Some");
        l.run = "dotnet fsi {file}";
    }
    {
        Language& l = add(v, "clojure", "Clojure");
        l.extensions = { "clj", "cljs", "cljc", "edn" };
        l.lineComment = ";";
        l.singleQuoteStrings = false;
        l.lispy = true;
        l.braces = false;
        l.keywords = words("def defn defn- defmacro defrecord defprotocol fn let if do when cond case loop recur "
                           "ns require import try catch finally throw quote");
        l.constants = words("true false nil");
        l.run = "clojure -M {file}";
    }
    {
        Language& l = add(v, "lisp", "Lisp");
        l.extensions = { "lisp", "lsp", "cl", "el", "scm", "ss", "rkt" };
        l.lineComment = ";";
        l.blockStart = "#|";
        l.blockEnd = "|#";
        l.singleQuoteStrings = false;
        l.lispy = true;
        l.braces = false;
        l.keywords = words("defun defmacro defvar defparameter defconstant define lambda let let* if cond when "
                           "unless loop progn setq setf quote and or not begin");
        l.constants = words("t nil #t #f");
        l.run = "sbcl --script {file}";
    }
    {
        Language& l = add(v, "nim", "Nim");
        l.extensions = { "nim", "nims" };
        l.lineComment = "#";
        l.blockStart = "#[";
        l.blockEnd = "]#";
        l.tripleQuotes = true;
        l.indentAfterColon = true;
        l.braces = false;
        l.keywords = words("addr and as asm bind block break case cast concept const continue converter defer "
                           "discard distinct div do elif else end enum except export finally for from func if "
                           "import in include interface is isnot iterator let macro method mixin mod nil not "
                           "notin object of or out proc ptr raise ref return shl shr static template try tuple "
                           "type using var when while xor yield");
        l.types = words("int int8 int16 int32 int64 uint float float32 float64 bool char string seq array");
        l.constants = words("true false nil");
        l.run = "nim c -r --hints:off {file}";
    }
    {
        Language& l = add(v, "crystal", "Crystal");
        l.extensions = { "cr" };
        l.lineComment = "#";
        l.braces = false;
        l.keywords = words("abstract alias annotation as begin break case class def do else elsif end ensure enum "
                           "extend for fun if in include lib macro module next of out private protected require "
                           "rescue return select self struct super then type typeof union unless until when "
                           "while with yield");
        l.constants = words("true false nil");
        l.run = "crystal run {file}";
    }
    {
        Language& l = add(v, "fortran", "Fortran");
        l.extensions = { "f", "f90", "f95", "f03", "f08", "for" };
        l.lineComment = "!";
        l.caseInsensitive = true;
        l.braces = false;
        l.keywords = words("program end module contains subroutine function call use implicit none integer real "
                           "double precision complex logical character dimension allocatable intent in out inout "
                           "if then else elseif do while select case print write read return stop parameter "
                           "type");
        l.constants = words(".true. .false.");
        l.build = "gfortran -g -O0 -Wall {file} -o {out}";
        l.run = "{out}";
        l.compiled = true;
        l.debugger = Language::Debugger::Lldb;
    }
    {
        Language& l = add(v, "pascal", "Pascal");
        l.extensions = { "pas", "pp", "dpr" };
        l.lineComment = "//";
        l.blockStart = "{";
        l.blockEnd = "}";
        l.caseInsensitive = true;
        l.braces = false;
        l.keywords = words("and array begin case const div do downto else end file for function goto if in "
                           "label mod nil not of or packed procedure program record repeat set then to type until "
                           "var while with uses unit interface implementation");
        l.types = words("integer real boolean char string byte word longint");
        l.constants = words("true false nil");
        l.build = "mkdir -p $(dirname {out}) && fpc -g -o{out} {file}";
        l.run = "{out}";
        l.compiled = true;
    }
    {
        Language& l = add(v, "asm", "Assembly");
        l.extensions = { "s", "asm", "nasm" };
        l.lineComment = ";";
        l.lineComment2 = "#";
        l.caseInsensitive = true;
        l.braces = false;
        l.keywords = words("mov add sub mul div inc dec push pop call ret jmp je jne jz jnz jg jl cmp test and or "
                           "xor not lea syscall int nop section global extern db dw dd dq ldr str bl b cbz");
    }
    {
        Language& l = add(v, "sql", "SQL");
        l.extensions = { "sql" };
        l.lineComment = "--";
        l.blockStart = "/*";
        l.blockEnd = "*/";
        l.caseInsensitive = true;
        l.braces = false;
        l.keywords = words("select from where and or not insert into values update set delete create table drop "
                           "alter index view join left right inner outer full on as group by order having limit "
                           "offset distinct union all case when then else end primary key foreign references "
                           "default unique check exists in like between is begin commit rollback transaction with");
        l.types = words("int integer bigint smallint real float double decimal numeric text varchar char boolean "
                        "date time timestamp blob serial");
        l.constants = words("null true false");
        l.run = "sqlite3 :memory: < {file}";
    }

    // --- Markup and data ------------------------------------------------------------------
    {
        Language& l = add(v, "html", "HTML");
        l.extensions = { "html", "htm", "xhtml", "vue", "svelte" };
        l.blockStart = "<!--";
        l.blockEnd = "-->";
        l.markup = true;
        l.braces = false;
#if defined(__APPLE__)
        l.run = "open {file}";
#else
        l.run = "xdg-open {file}";
#endif
    }
    {
        Language& l = add(v, "xml", "XML");
        l.extensions = { "xml", "svg", "plist", "xsd", "xsl", "csproj", "fsproj", "xib", "storyboard" };
        l.blockStart = "<!--";
        l.blockEnd = "-->";
        l.markup = true;
        l.braces = false;
    }
    {
        Language& l = add(v, "json", "JSON");
        l.extensions = { "json", "jsonc", "json5", "geojson" };
        l.lineComment = "//";
        l.singleQuoteStrings = false;
        l.constants = words("true false null");
    }
    {
        Language& l = add(v, "yaml", "YAML");
        l.extensions = { "yml", "yaml" };
        l.lineComment = "#";
        l.indentAfterColon = true;
        l.braces = false;
        l.constants = words("true false null yes no on off ~");
    }
    {
        Language& l = add(v, "toml", "TOML");
        l.extensions = { "toml", "ini", "cfg", "conf", "properties", "editorconfig" };
        l.filenames = { ".gitconfig", ".editorconfig" };
        l.lineComment = "#";
        l.lineComment2 = ";";
        l.tripleQuotes = true;
        l.braces = false;
        l.constants = words("true false");
    }
    {
        Language& l = add(v, "make", "Makefile");
        l.extensions = { "mk", "mak" };
        l.filenames = { "Makefile", "makefile", "GNUmakefile" };
        l.lineComment = "#";
        l.dollarVariables = true;
        l.useTabs = true;
        l.braces = false;
        l.keywords = words("ifeq ifneq ifdef ifndef else endif include define endef export override .PHONY");
    }
    {
        Language& l = add(v, "cmake", "CMake");
        l.extensions = { "cmake" };
        l.filenames = { "CMakeLists.txt" };
        l.lineComment = "#";
        l.dollarVariables = true;
        l.caseInsensitive = true;
        l.braces = false;
        l.keywords = words("cmake_minimum_required project add_executable add_library add_subdirectory "
                           "target_link_libraries target_include_directories target_compile_options "
                           "target_compile_definitions target_sources find_package include set option if elseif "
                           "else endif foreach endforeach function endfunction macro endmacro message install "
                           "list string file add_custom_command add_custom_target add_dependencies "
                           "set_target_properties fetchcontent_declare fetchcontent_makeavailable enable_testing "
                           "add_test");
        l.constants = words("ON OFF TRUE FALSE PUBLIC PRIVATE INTERFACE REQUIRED");
    }
    {
        Language& l = add(v, "docker", "Dockerfile");
        l.filenames = { "Dockerfile", "Containerfile" };
        l.extensions = { "dockerfile" };
        l.lineComment = "#";
        l.dollarVariables = true;
        l.caseInsensitive = true;
        l.braces = false;
        l.keywords = words("from as run cmd label expose env add copy entrypoint volume user workdir arg onbuild "
                           "stopsignal healthcheck shell");
    }
    {
        Language& l = add(v, "markdown", "Markdown");
        l.extensions = { "md", "markdown", "mdx" };
        l.braces = false;
        l.singleQuoteStrings = false;
    }
    {
        Language& l = add(v, "graphql", "GraphQL");
        l.extensions = { "graphql", "gql" };
        l.lineComment = "#";
        l.tripleQuotes = true;
        l.keywords = words("query mutation subscription fragment on type interface union enum input scalar "
                           "schema extend implements directive");
        l.types = words("Int Float String Boolean ID");
        l.constants = words("true false null");
    }
    {
        Language& l = add(v, "latex", "LaTeX");
        l.extensions = { "tex", "sty", "cls", "bib" };
        l.lineComment = "%";
        l.singleQuoteStrings = false;
        l.braces = false;
    }
    return v;
}

const std::vector<Language>& registry()
{
    static const std::vector<Language> langs = build();
    return langs;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

const std::vector<Language>& allLanguages()
{
    return registry();
}

const Language& plainText()
{
    static Language plain = [] {
        Language l;
        l.id = "text";
        l.name = "Plain Text";
        l.braces = false;
        l.singleQuoteStrings = false;
        return l;
    }();
    return plain;
}

const Language& languageForFile(const std::string& path, const std::string& firstLine)
{
    const auto& langs = registry();
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const auto& l : langs)
        for (const auto& f : l.filenames)
            if (name == f) return l;

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < name.size())
    {
        std::string ext = lower(name.substr(dot + 1));
        // .h is shared by C and C++; C++ is the safer guess for highlighting.
        if (ext == "h") ext = "hpp";
        for (const auto& l : langs)
            for (const auto& e : l.extensions)
                if (ext == e) return l;
    }
    if (name.rfind("Dockerfile", 0) == 0) return languageForFile("Dockerfile");

    // "#!/usr/bin/env python3" or "#!/bin/bash"
    if (firstLine.rfind("#!", 0) == 0)
    {
        std::string line = firstLine.substr(2);
        std::string interp;
        size_t p = line.find_last_of('/');
        std::string rest = p == std::string::npos ? line : line.substr(p + 1);
        size_t sp = rest.find(' ');
        interp = rest.substr(0, sp);
        if (interp == "env" && sp != std::string::npos)
        {
            std::string args = rest.substr(sp + 1);
            while (!args.empty() && args[0] == '-')  // env -S
            {
                size_t n = args.find(' ');
                args = n == std::string::npos ? "" : args.substr(n + 1);
            }
            interp = args.substr(0, args.find(' '));
        }
        // python3.12 -> python3
        while (!interp.empty() && (std::isdigit((unsigned char)interp.back()) || interp.back() == '.') &&
               interp != "python3" && interp != "python2")
            interp.pop_back();
        for (const auto& l : langs)
            for (const auto& s : l.shebangs)
                if (interp == s) return l;
    }
    return plainText();
}
