#pragma once

#include <string>
#include <vector>

// Splits a Verse function signature into the pieces Godot's documentation draws separately.
//
// The input is what the host's SpellSignature produces and what `vh_complete_item::Signature`
// carries: a parameter list, the effect specifiers, and the result type, spelled the way Verse
// spells them and with no default values --
//
//     (Body:node2d)<transacts>:void
//     (X:float, Y:float):vector2
//     (Pred:(:int)<decides>->logic)<transacts>:void
//     ():int
//
// A DocData::MethodDoc wants the arguments and the result type apart, because Godot renders the
// name, then each `argument.name: argument.type`, then `-> return_type` (editor_help.cpp's
// SYMBOL_HINT_SIGNATURE). Handing it the whole function type as `return_type` -- which is what
// _get_documentation did before this -- drew `Name() -> params->result`, with no arguments and the
// signature standing in for the return type.
//
// Every split is at the top level: a parameter type may itself hold parentheses (a function type),
// brackets (`[]int`), or braces, so commas and colons inside those are not separators. The type
// text is kept verbatim, Verse spelling and all, because the argument hint beside the tooltip is
// spelled the same way and the two should not disagree.

struct VerseSignatureParam {
	std::string name;
	std::string type;
};

struct VerseSignature {
	std::vector<VerseSignatureParam> params;
	// The effect specifiers with their angle brackets removed and joined by spaces: `transacts`,
	// `suspends decides`, or empty for the default effect set. Godot draws an unknown qualifier as
	// plain text after the signature, which is where a Verse effect reads acceptably.
	std::string specifiers;
	std::string result_type;
};

VerseSignature verse_parse_signature(const std::string &p_signature);
