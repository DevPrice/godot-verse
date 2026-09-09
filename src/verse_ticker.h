#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

// Drives one compiled Verse script from the scene tree: compiles verse_script in _ready, then
// pumps vh_tick and calls Ready()/Update(:float) when the script defines them.
class VerseTicker : public godot::Node {
	GDCLASS(VerseTicker, godot::Node)

protected:
	static void _bind_methods();

public:
	VerseTicker() = default;
	~VerseTicker() override = default;

	void set_verse_script(const godot::String &p_path);
	godot::String get_verse_script() const;

	void set_budget_ms(double p_budget_ms);
	double get_budget_ms() const;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

private:
	godot::String verse_script;
	double budget_ms = 4.0;

	bool script_loaded = false;
	bool has_ready_fn = false;
	bool has_update_fn = false;
};
