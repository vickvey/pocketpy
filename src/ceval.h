#pragma once

#include "common.h"
#include "vm.h"

namespace pkpy{

inline PyObject* VM::run_frame(Frame* frame){
    while(true){
        /* NOTE: 
        * Be aware of accidental gc!
        * DO NOT leave any strong reference of PyObject* in the C stack
        * For example, frame->popx() returns a strong reference which may be dangerous
        * `Args` containing strong references is safe if it is passed to `call` or `fast_call`
        */
        heap._auto_collect(this);

        const Bytecode& byte = frame->next_bytecode();
        switch (byte.op)
        {
        case OP_NO_OP: continue;
        /*****************************************/
        case OP_POP_TOP: frame->pop(); continue;
        case OP_DUP_TOP: frame->push(frame->top()); continue;
        case OP_ROT_TWO: std::swap(frame->top(), frame->top_1()); continue;
        case OP_PRINT_EXPR: {
            PyObject* obj = frame->top();  // use top() to avoid accidental gc
            if(obj != None) *_stdout << CAST(Str, asRepr(obj)) << '\n';
            frame->pop();
        } continue;
        /*****************************************/
        case OP_LOAD_CONST: frame->push(frame->co->consts[byte.arg]); continue;
        case OP_LOAD_NONE: frame->push(None); continue;
        case OP_LOAD_TRUE: frame->push(True); continue;
        case OP_LOAD_FALSE: frame->push(False); continue;
        case OP_LOAD_ELLIPSIS: frame->push(Ellipsis); continue;
        case OP_LOAD_BUILTIN_EVAL: frame->push(builtins->attr(m_eval)); continue;
        case OP_LOAD_FUNCTION: {
            PyObject* obj = frame->co->consts[byte.arg];
            Function f = CAST(Function, obj);   // copy it!
            f._module = frame->_module;         // setup module
            frame->push(VAR(std::move(f)));
        } continue;
        /*****************************************/
        case OP_LOAD_NAME: {
            StrName name = frame->co->names[byte.arg];
            PyObject* val;
            int i = 0;  // names[0] is ensured to be non-null
            do{
                val = frame->names[i++]->try_get(name);
                if(val != nullptr){ frame->push(val); break; }
            }while(frame->names[i] != nullptr);
            vm->NameError(name);
        } continue;
        case OP_LOAD_ATTR: {
            PyObject* a = frame->top();
            StrName name = frame->co->names[byte.arg];
            frame->top() = getattr(a, name);
        } continue;
        case OP_LOAD_SUBSCR: {
            Args args(2);
            args[1] = frame->popx();    // b
            args[0] = frame->top();     // a
            frame->top() = fast_call(__getitem__, std::move(args));
        } continue;
        case OP_STORE_LOCAL: {
            StrName name = frame->co->names[byte.arg];
            frame->f_locals().set(name, frame->popx());
        } continue;
        case OP_STORE_GLOBAL: {
            StrName name = frame->co->names[byte.arg];
            frame->f_globals().set(name, frame->popx());
        } continue;
        case OP_STORE_ATTR: {
            StrName name = frame->co->names[byte.arg];
            PyObject* a = frame->top();
            PyObject* val = frame->top_1();
            setattr(a, name, val);
            frame->pop_n(2);
        } continue;
        case OP_STORE_SUBSCR: {
            Args args(3);
            args[1] = frame->popx();    // b
            args[0] = frame->popx();    // a
            args[2] = frame->popx();    // val
            fast_call(__setitem__, std::move(args));
        } continue;
        case OP_DELETE_LOCAL: {
            StrName name = frame->co->names[byte.arg];
            if(frame->f_locals().contains(name)){
                frame->f_locals().erase(name);
            }else{
                NameError(name);
            }
        } continue;
        case OP_DELETE_GLOBAL: {
            StrName name = frame->co->names[byte.arg];
            if(frame->f_globals().contains(name)){
                frame->f_globals().erase(name);
            }else{
                NameError(name);
            }
        } continue;
        case OP_DELETE_ATTR: {
            PyObject* a = frame->popx();
            StrName name = frame->co->names[byte.arg];
            if(!a->is_attr_valid()) TypeError("cannot delete attribute");
            if(!a->attr().contains(name)) AttributeError(a, name);
            a->attr().erase(name);
        } continue;
        case OP_DELETE_SUBSCR: {
            PyObject* b = frame->popx();
            PyObject* a = frame->popx();
            fast_call(__delitem__, Args{a, b});
        } continue;
        /*****************************************/
        case OP_BUILD_LIST:
            frame->push(VAR(frame->popx_n_reversed(byte.arg).to_list()));
            continue;
        case OP_BUILD_DICT: {
            PyObject* t = VAR(frame->popx_n_reversed(byte.arg));
            PyObject* obj = call(builtins->attr(m_dict), Args{t});
            frame->push(obj);
        } continue;
        case OP_BUILD_SET: {
            PyObject* t = VAR(frame->popx_n_reversed(byte.arg));
            PyObject* obj = call(builtins->attr(m_set), Args{t});
            frame->push(obj);
        } continue;
        case OP_BUILD_SLICE: {
            PyObject* step = frame->popx();
            PyObject* stop = frame->popx();
            PyObject* start = frame->popx();
            Slice s;
            if(start != None) s.start = CAST(int, start);
            if(stop != None) s.stop = CAST(int, stop);
            if(step != None) s.step = CAST(int, step);
            frame->push(VAR(s));
        } continue;
        case OP_BUILD_TUPLE: {
            Tuple items = frame->popx_n_reversed(byte.arg);
            frame->push(VAR(std::move(items)));
        } continue;
        case OP_BUILD_STRING: {
            // asStr() may run extra bytecode
            // so we use top_n_reversed() in order to avoid accidental gc
            Args items = frame->top_n_reversed(byte.arg);
            StrStream ss;
            for(int i=0; i<items.size(); i++) ss << CAST(Str, asStr(items[i]));
            frame->pop_n(byte.arg);
            frame->push(VAR(ss.str()));
        } continue;
        /*****************************************/
        case OP_BINARY_OP: {
            Args args(2);
            args[1] = frame->popx();    // lhs
            args[0] = frame->top();     // rhs
            frame->top() = fast_call(BINARY_SPECIAL_METHODS[byte.arg], std::move(args));
        } continue;
        case OP_COMPARE_OP: {
            Args args(2);
            args[1] = frame->popx();    // lhs
            args[0] = frame->top();     // rhs
            frame->top() = fast_call(COMPARE_SPECIAL_METHODS[byte.arg], std::move(args));
        } continue;
        case OP_BITWISE_OP: {
            Args args(2);
            args[1] = frame->popx();    // lhs
            args[0] = frame->top();     // rhs
            frame->top() = fast_call(BITWISE_SPECIAL_METHODS[byte.arg], std::move(args));
        } continue;
        case OP_IS_OP: {
            PyObject* rhs = frame->popx();
            PyObject* lhs = frame->top();
            bool ret_c = lhs == rhs;
            if(byte.arg == 1) ret_c = !ret_c;
            frame->top() = VAR(ret_c);
        } continue;
        case OP_CONTAINS_OP: {
            Args args(2);
            args[0] = frame->popx();
            args[1] = frame->top();
            PyObject* ret = fast_call(__contains__, std::move(args));
            bool ret_c = CAST(bool, ret);
            if(byte.arg == 1) ret_c = !ret_c;
            frame->top() = VAR(ret_c);
        } continue;
        /*****************************************/
        case OP_JUMP_ABSOLUTE: frame->jump_abs(byte.arg); continue;
        case OP_POP_JUMP_IF_FALSE:
            if(!asBool(frame->popx())) frame->jump_abs(byte.arg);
            continue;
        case OP_JUMP_IF_TRUE_OR_POP:
            if(asBool(frame->top()) == true) frame->jump_abs(byte.arg);
            else frame->pop();
            continue;
        case OP_JUMP_IF_FALSE_OR_POP:
            if(asBool(frame->top()) == false) frame->jump_abs(byte.arg);
            else frame->pop();
            continue;
        case OP_LOOP_CONTINUE: {
            int target = frame->co->blocks[byte.block].start;
            frame->jump_abs(target);
        } continue;
        case OP_LOOP_BREAK: {
            int target = frame->co->blocks[byte.block].end;
            frame->jump_abs_break(target);
        } continue;
        case OP_GOTO: {
            StrName label = frame->co->names[byte.arg];
            auto it = frame->co->labels.find(label);
            if(it == frame->co->labels.end()) _error("KeyError", "label " + label.str().escape(true) + " not found");
            frame->jump_abs_break(it->second);
        } continue;
        /*****************************************/
        // TODO: examine this later
        case OP_CALL: case OP_CALL_UNPACK: {
            Args args = frame->popx_n_reversed(byte.arg);
            if(byte.op == OP_CALL_UNPACK) unpack_args(args);
            PyObject* callable = frame->popx();
            PyObject* ret = call(callable, std::move(args), no_arg(), true);
            if(ret == _py_op_call) return ret;
            frame->push(std::move(ret));
        } continue;
        case OP_CALL_KWARGS: case OP_CALL_KWARGS_UNPACK: {
            int ARGC = byte.arg & 0xFFFF;
            int KWARGC = (byte.arg >> 16) & 0xFFFF;
            Args kwargs = frame->popx_n_reversed(KWARGC*2);
            Args args = frame->popx_n_reversed(ARGC);
            if(byte.op == OP_CALL_KWARGS_UNPACK) unpack_args(args);
            PyObject* callable = frame->popx();
            PyObject* ret = call(callable, std::move(args), kwargs, true);
            if(ret == _py_op_call) return ret;
            frame->push(std::move(ret));
        } continue;
        case OP_RETURN_VALUE: return frame->popx();
        /*****************************************/
        case OP_LIST_APPEND: {
            PyObject* obj = frame->popx();
            List& list = CAST(List&, frame->top_1());
            list.push_back(obj);
        } continue;
        case OP_DICT_ADD: {
            PyObject* kv = frame->popx();
            // we do copy here to avoid accidental gc in `kv`
            // TODO: optimize to avoid copy
            call(frame->top_1(), __setitem__, CAST(Tuple, kv));
        } continue;
        case OP_SET_ADD: {
            PyObject* obj = frame->popx();
            call(frame->top_1(), m_add, Args{obj});
        } continue;
        /*****************************************/
        case OP_UNARY_NEGATIVE:
            frame->top() = num_negated(frame->top());
            continue;
        case OP_UNARY_NOT:
            frame->top() = VAR(!asBool(frame->top()));
            continue;
        case OP_UNARY_STAR:
            frame->top() = VAR(StarWrapper(frame->top()));
            continue;
        /*****************************************/
        case OP_GET_ITER:
            frame->top() = asIter(frame->top());
            continue;
        case OP_FOR_ITER: {
            BaseIter* it = PyIter_AS_C(frame->top());
            PyObject* obj = it->next();
            if(obj != nullptr){
                frame->push(obj);
            }else{
                int target = frame->co->blocks[byte.block].end;
                frame->jump_abs_break(target);
            }
        } continue;
        /*****************************************/
        case OP_IMPORT_NAME: {
            StrName name = frame->co->names[byte.arg];
            PyObject* ext_mod = _modules.try_get(name);
            if(ext_mod == nullptr){
                Str source;
                auto it = _lazy_modules.find(name);
                if(it == _lazy_modules.end()){
                    bool ok = false;
                    source = _read_file_cwd(name.str() + ".py", &ok);
                    if(!ok) _error("ImportError", "module " + name.str().escape(true) + " not found");
                }else{
                    source = it->second;
                    _lazy_modules.erase(it);
                }
                CodeObject_ code = compile(source, name.str(), EXEC_MODE);
                PyObject* new_mod = new_module(name);
                _exec(code, new_mod);
                new_mod->attr()._try_perfect_rehash();
            }
            frame->push(ext_mod);
        } continue;
        case OP_IMPORT_STAR: {
            PyObject* obj = frame->popx();
            for(auto& [name, value]: obj->attr().items()){
                Str s = name.str();
                if(s.empty() || s[0] == '_') continue;
                frame->f_globals().set(name, value);
            }
        }; continue;
        /*****************************************/
        /*****************************************/
        // case OP_SETUP_DECORATOR: continue;
        // case OP_SETUP_CLOSURE: {
        //     Function& f = CAST(Function&, frame->top());    // reference
        //     f._closure = frame->_locals;
        // } continue;
        // case OP_BEGIN_CLASS: {
        //     StrName name = frame->co->names[byte.arg];
        //     PyObject* clsBase = frame->popx();
        //     if(clsBase == None) clsBase = _t(tp_object);
        //     check_type(clsBase, tp_type);
        //     PyObject* cls = new_type_object(frame->_module, name, OBJ_GET(Type, clsBase));
        //     frame->push(cls);
        // } continue;
        // case OP_END_CLASS: {
        //     PyObject* cls = frame->popx();
        //     cls->attr()._try_perfect_rehash();
        // }; continue;
        // case OP_STORE_CLASS_ATTR: {
        //     StrName name = frame->co->names[byte.arg];
        //     PyObject* obj = frame->popx();
        //     PyObject* cls = frame->top();
        //     cls->attr().set(name, obj);
        // } continue;
        // case OP_ASSERT: {
        //     PyObject* _msg = frame->pop_value(this);
        //     Str msg = CAST(Str, asStr(_msg));
        //     PyObject* expr = frame->pop_value(this);
        //     if(asBool(expr) != True) _error("AssertionError", msg);
        // } continue;
        // case OP_EXCEPTION_MATCH: {
        //     const auto& e = CAST(Exception&, frame->top());
        //     StrName name = frame->co->names[byte.arg].first;
        //     frame->push(VAR(e.match_type(name)));
        // } continue;
        // case OP_RAISE: {
        //     PyObject* obj = frame->pop_value(this);
        //     Str msg = obj == None ? "" : CAST(Str, asStr(obj));
        //     StrName type = frame->co->names[byte.arg].first;
        //     _error(type, msg);
        // } continue;
        // case OP_RE_RAISE: _raise(); continue;
        // case OP_YIELD_VALUE: return _py_op_yield;
        // // TODO: using "goto" inside with block may cause __exit__ not called
        // case OP_WITH_ENTER: call(frame->pop_value(this), __enter__, no_arg()); continue;
        // case OP_WITH_EXIT: call(frame->pop_value(this), __exit__, no_arg()); continue;
        // case OP_TRY_BLOCK_ENTER: frame->on_try_block_enter(); continue;
        // case OP_TRY_BLOCK_EXIT: frame->on_try_block_exit(); continue;
        default: throw std::runtime_error(Str("opcode ") + OP_NAMES[byte.op] + " is not implemented");
        }
    }
    UNREACHABLE();
}

} // namespace pkpy