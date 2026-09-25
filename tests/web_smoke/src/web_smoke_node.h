#ifndef WEB_SMOKE_NODE_H
#define WEB_SMOKE_NODE_H

#include <godot_cpp/classes/node.hpp>

namespace godot {

class WebSmokeNode : public Node {
	GDCLASS(WebSmokeNode, Node)

protected:
	static void _bind_methods();

public:
	void _ready() override;
};

} // namespace godot

#endif // WEB_SMOKE_NODE_H
