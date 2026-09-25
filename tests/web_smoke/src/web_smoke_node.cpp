#include "web_smoke_node.h"

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void WebSmokeNode::_bind_methods() {
}

void WebSmokeNode::_ready() {
	UtilityFunctions::print("web_smoke: ready");
}
