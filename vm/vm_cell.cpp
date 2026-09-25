#include "vm_cell.h"

#include <initializer_list>

namespace vm {

const char *cell_kind_name(CellKind p_kind) {
	switch (p_kind) {
		case CellKind::False:
			return "false";
		case CellKind::True:
			return "true";
		case CellKind::BuiltinPackage:
			return "builtin package";
		case CellKind::Name:
			return "name";
		case CellKind::Array:
			return "array";
		case CellKind::MutableArray:
			return "mutable array";
		case CellKind::Map:
			return "map";
		case CellKind::MutableMap:
			return "mutable map";
		case CellKind::Option:
			return "option";
		case CellKind::HeapInt:
			return "heap int";
		case CellKind::Rational:
			return "rational";
		case CellKind::Procedure:
			return "procedure";
		case CellKind::NativeProcedure:
			return "native procedure";
		case CellKind::Function:
			return "function";
		case CellKind::Scope:
			return "scope";
		case CellKind::Class:
			return "class";
		case CellKind::Archetype:
			return "archetype";
		case CellKind::AccessSpecifier:
			return "access specifier";
		case CellKind::Enumeration:
			return "enumeration";
		case CellKind::Enumerator:
			return "enumerator";
		case CellKind::Package:
			return "package";
		case CellKind::Module:
			return "module";
		case CellKind::Object:
			return "object";
		case CellKind::IntType:
			return "int type";
		case CellKind::FloatType:
			return "float type";
		case CellKind::TupleType:
			return "tuple type";
		case CellKind::MapType:
			return "map type";
		case CellKind::SimpleType:
			return "simple type";
		case CellKind::Accessor:
			return "accessor";
		case CellKind::ArrayType:
			return "array type";
		case CellKind::OptionType:
			return "option type";
		case CellKind::PointerType:
			return "pointer type";
		case CellKind::Placeholder:
			return "placeholder";
		case CellKind::Ref:
			return "ref";
		case CellKind::Frame:
			return "frame";
		case CellKind::Task:
			return "task";
		case CellKind::NativeObject:
			return "native object";
		case CellKind::AccessorRef:
			return "accessor reference";
		case CellKind::SetterChain:
			return "construction token";
		case CellKind::Semaphore:
			return "semaphore";
		case CellKind::ContentScope:
			return "content scope";
	}
	return "unknown";
}

void ArrayCell::spread_to_values() {
	if (storage == Storage::Values) {
		return;
	}
	values.reserve(bytes.size());
	for (char byte : bytes) {
		values.push_back(Value::from_char8(uint8_t(byte)));
	}
	bytes.clear();
	bytes.shrink_to_fit();
	storage = Storage::Values;
}

void ArrayCell::set(size_t p_index, Value p_value) {
	if (storage == Storage::Char8) {
		if (p_value.is_char8()) {
			bytes[p_index] = char(p_value.as_char8());
			return;
		}
		spread_to_values();
	}
	values[p_index] = p_value;
}

void ArrayCell::append(Value p_value) {
	if (length() == 0) {
		storage = p_value.is_char8() ? Storage::Char8 : Storage::Values;
	}
	if (storage == Storage::Char8) {
		if (p_value.is_char8()) {
			bytes.push_back(char(p_value.as_char8()));
			return;
		}
		spread_to_values();
	}
	values.push_back(p_value);
}

void ArrayCell::truncate(size_t p_length) {
	if (storage == Storage::Char8) {
		bytes.resize(p_length);
	} else {
		values.resize(p_length);
	}
}

void ClassCell::visit_references(CellVisitor &r_visitor) const {
	r_visitor.visit(package);
	r_visitor.visit(attributes);
	for (const ClassCell *base : inherited) {
		r_visitor.visit(base);
	}
	r_visitor.visit(archetype);
	r_visitor.visit(constructor);
	r_visitor.visit(blocks);
}

void TaskCell::visit_references(CellVisitor &r_visitor) const {
	r_visitor.visit(result);
	r_visitor.visit(parent);
	for (const std::vector<TaskCell *> *list : { &children, &awaiters, &cancelers }) {
		for (const TaskCell *task : *list) {
			r_visitor.visit(task);
		}
	}
	r_visitor.visit(resume_frame);
	r_visitor.visit(yield_task);
	r_visitor.visit(yield_frame);
	for (const std::vector<TaskHook> *hooks : { &defer_hooks, &finish_hooks }) {
		for (const TaskHook &hook : *hooks) {
			r_visitor.visit(hook.target);
		}
	}
	r_visitor.visit(group);
	r_visitor.visit(captured_scope);
}

void EnumerationCell::visit_references(CellVisitor &r_visitor) const {
	r_visitor.visit(name);
	for (const EnumeratorCell *enumerator : enumerators) {
		r_visitor.visit(enumerator);
	}
}

} // namespace vm
