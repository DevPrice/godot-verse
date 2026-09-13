#pragma once

#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

// A Godot Callable that calls a Verse function (R-TYPE-3's other direction, R-INT-4, R-SIG-3).
//
// The mirror image of the reference table: that one lets Verse hold a Godot value, this lets Godot
// hold a Verse one. What it holds is a callback id the host minted, plus the instance id of the
// node the Verse function is bound to.
//
// **Only a function bound to a script instance** is accepted, and the reason is Godot's own design
// rather than caution. GDScript answers the same question twice, differently:
// GDScriptLambdaSelfCallable reports the captured object and dies with it;
// GDScriptLambdaCallable reports the *script resource*, overrides is_valid() to "the function
// exists", and so outlives the object -- which is Godot's own known leak (GH-102327, and the
// exit-time segfault phase-2-design.md 11 met from the other end). Binding to the node is the half
// that does not leak: get_object() is the node, is_valid() is ObjectDB's answer, and a freed node
// drops its connections with no work from us.
//
// Equality is by reference, which is what both prior arts do -- Godot's own lambda callables
// compare `p_a == p_b` ("Lambda callables are only compared by reference"), and UEFN's event
// inserts one entry per subscribe. So two subscriptions of one handler are two connections with
// two independent cancels, rather than the duplicate Godot refuses for `Callable(node, "method")`.
class VerseCallable : public godot::CallableCustom {
public:
	// Mints a Callable over p_callback_id. The id is released back to the host when the last
	// reference to the Callable goes, which is the only moment "Godot has finished with this" is
	// knowable from here.
	static godot::Callable make(int64_t p_callback_id, int64_t p_owner_id);

	~VerseCallable() override;

	uint32_t hash() const override;
	godot::String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override;
	CompareLessFunc get_compare_less_func() const override;
	bool is_valid() const override;
	godot::ObjectID get_object() const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;

private:
	VerseCallable(int64_t p_callback_id, int64_t p_owner_id) :
			callback_id(p_callback_id), owner_id(p_owner_id) {}

	static bool compare_equal(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b);
	static bool compare_less(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b);

	int64_t callback_id = 0;
	int64_t owner_id = 0;
};
