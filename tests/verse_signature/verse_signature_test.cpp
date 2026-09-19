// Standalone driver for the Verse signature parser. No Godot and no godot-cpp, like the lexer and
// doc-markup tests beside it: splitting a signature string is decidable from the text alone. Each
// case is one shape the host's SpellSignature can hand back, and the expectation is the argument
// list, the effect specifiers, and the result type Godot's documentation draws separately.
#include "verse_signature.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

bool Step(const char *Name, bool Result) {
	printf("[verse_signature_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	if (!Result) {
		g_failures++;
	}
	return Result;
}

std::string Render(const VerseSignature &Sig) {
	// A single line so a mismatch prints readably: `Name:type, Name2:type2 | specs -> result`.
	std::string out;
	for (size_t i = 0; i < Sig.params.size(); i++) {
		if (i > 0) {
			out += ", ";
		}
		out += Sig.params[i].name + ":" + Sig.params[i].type;
	}
	out += " | " + Sig.specifiers + " -> " + Sig.result_type;
	return out;
}

bool Expect(const char *Name, const std::string &Signature, const std::string &Want) {
	const std::string Got = Render(verse_parse_signature(Signature));
	const bool Ok = Got == Want;
	if (!Ok) {
		printf("    want: %s\n    got:  %s\n", Want.c_str(), Got.c_str());
	}
	return Step(Name, Ok);
}

} // namespace

int main() {
	Expect("no parameters, a plain result", "():int", " |  -> int");
	Expect("one parameter", "(Body:node2d)<transacts>:void", "Body:node2d | transacts -> void");
	// Two parameters with no specifiers: nothing between `)` and `:`.
	Expect("two parameters, no specifiers", "(X:float, Y:float):vector2", "X:float, Y:float |  -> vector2");
	Expect("a failable call keeps decides as a specifier",
			"(Index:int)<decides>:node", "Index:int | decides -> node");
	Expect("two specifiers join with a space",
			"(Val:int)<suspends><decides>:node", "Val:int | suspends decides -> node");
	Expect("a bracketed type is one parameter",
			"(Items:[]int):int", "Items:[]int |  -> int");
	Expect("a comma inside a type is not a separator",
			"(Data:tuple(int, float)):int", "Data:tuple(int, float) |  -> int");
	Expect("a function-type parameter's colon is not the name divider",
			"(Pred:(:int)<decides>->logic)<transacts>:void",
			"Pred:(:int)<decides>->logic | transacts -> void");
	Expect("a function-type result keeps its own colon",
			"(X:int):(:int)->logic", "X:int |  -> (:int)->logic");
	Expect("spacing around a parameter is trimmed",
			"( Name : int ):void", "Name:int |  -> void");
	// A malformed signature yields nothing rather than a crash.
	Expect("no parentheses is empty", "not a signature", " |  -> ");

	printf("[verse_signature_test] %s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
	return g_failures == 0 ? 0 : 1;
}
