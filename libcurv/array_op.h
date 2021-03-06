// Copyright 2016-2019 Doug Moen
// Licensed under the Apache License, version 2.0
// See accompanying file LICENSE or https://www.apache.org/licenses/LICENSE-2.0

#ifndef LIBCURV_ARRAY_OP_H
#define LIBCURV_ARRAY_OP_H

// Curv is an array language, following APL and its successors.
// This means that scalar operations (on numbers and booleans)
// are generalized to operate on arrays of scalars, in two ways:
// element-wise operation, and broadcasting.
//   2 + 2 == 4                  -- a scalar operation
//   [3,4] + [10,20] == [13,24]  -- element-wise addition
//   1 + [10,20] == [11,21]      -- broadcasting

#include <libcurv/context.h>
#include <libcurv/exception.h>
#include <libcurv/list.h>
#include <libcurv/reactive.h>
#include <libcurv/sc_compiler.h>
#include <libcurv/sc_context.h>
#include <libcurv/typeconv.h>

namespace curv {

template <class Scalar_Op>
struct Binary_Numeric_Array_Op
{
    // TODO: optimize: move semantics. unique object reuse.

    static Value
    reduce(const Scalar_Op& f, double zero, Value arg)
    {
        auto list = arg.to<List>(f.cx);
        Value result = {zero};
        for (auto val : *list)
            result = op(f, result, val);
        return result;
    }

    static Value
    op(const Scalar_Op& f, Value x, Value y)
    {
        // if both x and y are numbers
        double r = f.call(x.to_num_or_nan(), y.to_num_or_nan());
        if (r == r)
            return {r};

        // if x, y, or both, are lists
        if (auto xlist = x.dycast<List>()) {
            if (auto ylist = y.dycast<List>())
                return {element_wise_op(f, xlist, ylist)};
            return {broadcast_left(f, xlist, y)};
        }
        if (auto ylist = y.dycast<List>())
            return {broadcast_right(f, x, ylist)};

        // One of x or y is reactive, the other is a number.
        // Both x and y are reactive.
        if (x.is_num()) {
            auto yre = y.dycast<Reactive_Value>();
            if (yre && yre->sctype_ == SC_Type::Num()) {
                auto& syn = f.cx.syntax();
                return {make<Reactive_Expression>(
                    SC_Type::Num(),
                    f.make_expr(
                        make<Constant>(share(syn), x),
                        yre->expr(syn)),
                    f.cx)};
            }
        }
        auto xre = x.dycast<Reactive_Value>();
        if (xre && xre->sctype_ == SC_Type::Num()) {
            auto& syn = f.cx.syntax();
            if (y.is_num())
                return {make<Reactive_Expression>(
                    SC_Type::Num(),
                    f.make_expr(xre->expr(syn),
                        make<Constant>(share(syn), y)),
                    f.cx)};
            auto yre = y.dycast<Reactive_Value>();
            if (yre && yre->sctype_ == SC_Type::Num())
                return {make<Reactive_Expression>(
                    SC_Type::Num(),
                    f.make_expr(xre->expr(syn), yre->expr(syn)),
                    f.cx)};
        }

        throw Exception(f.cx,
            stringify(f.callstr(x,y),": domain error"));
    }

    static Shared<List>
    broadcast_left(const Scalar_Op& f, Shared<List> xlist, Value y)
    {
        Shared<List> result = List::make(xlist->size());
        for (unsigned i = 0; i < xlist->size(); ++i)
            (*result)[i] = op(f, (*xlist)[i], y);
        return result;
    }

    static Shared<List>
    broadcast_right(const Scalar_Op& f, Value x, Shared<List> ylist)
    {
        Shared<List> result = List::make(ylist->size());
        for (unsigned i = 0; i < ylist->size(); ++i)
            (*result)[i] = op(f, x, (*ylist)[i]);
        return result;
    }

    static Shared<List>
    element_wise_op(const Scalar_Op& f, Shared<List> xs, Shared<List> ys)
    {
        if (xs->size() != ys->size())
            throw Exception(f.cx, stringify(
                "mismatched list sizes (",
                xs->size(),",",ys->size(),") in array operation"));
        Shared<List> result = List::make(xs->size());
        for (unsigned i = 0; i < xs->size(); ++i)
            (*result)[i] = op(f, (*xs)[i], (*ys)[i]);
        return result;
    }
};

// This Prim accepts arbitrary non-List arguments.
struct Unary_Scalar_Prim
{
    typedef Value scalar_t;
    static bool unbox(Value a, scalar_t& b, const Context&)
    {
        if (a.dycast<List>()) {
            return false;
        } else {
            b = a;
            return true;
        }
    }
    static void sc_check_arg(SC_Value a, const Context& cx)
    {
        if (!a.type.is_struc())
            throw Exception(cx, "argument must be a Struc");
    }
};

// This Prim accepts a pair of arbitrary non-List arguments.
struct Binary_Scalar_Prim : public Unary_Scalar_Prim
{
    typedef Value left_t, right_t;
    static bool unbox_left(Value a, left_t& b, const Context& cx)
    {
        return unbox(a, b, cx);
    }
    static bool unbox_right(Value a, right_t& b, const Context& cx)
    {
        return unbox(a, b, cx);
    }
    static void sc_check_args(
        SC_Frame& f, SC_Value& a, SC_Value& b, const Context& cx)
    {
        sc_struc_unify(f, a, b, cx);
    }
};

// This Prim accepts Bool but not Bool32 arguments in SubCurv.
struct Unary_Bool_Prim
{
    typedef bool scalar_t;
    static bool unbox(Value a, scalar_t& b, const Context& cx)
    {
        if (a.is_bool()) {
            b = a.to_bool_unsafe();
            return true;
        } else
            return false;
    }
    static void sc_check_arg(SC_Value a, const Context& cx)
    {
        if (a.type.is_bool_or_vec()) return;
        throw Exception(cx, "argument must be Bool or BVec");
    }
};

// This Prim accepts Bool and Bool32 arguments in SubCurv.
struct Binary_Bool_Or_Bool32_Prim
{
    typedef bool left_t, right_t;
    static bool unbox_left(Value a, left_t& b, const Context&)
    {
        if (a.is_bool()) {
            b = a.to_bool_unsafe();
            return true;
        } else
            return false;
    }
    static bool unbox_right(Value a, right_t& b, const Context&)
    {
        if (a.is_bool()) {
            b = a.to_bool_unsafe();
            return true;
        } else
            return false;
    }
    static void sc_check_arg(SC_Value a, const Context& cx)
    {
        if (a.type.is_bool_struc()) return;
        throw Exception(cx, "argument must be Bool or Bool32");
    }
    static void sc_check_args(
        SC_Frame& /*f*/, SC_Value& a, SC_Value& b, const Context& cx)
    {
        if (a.type.is_bool_or_vec()) {
            if (b.type.is_bool_or_vec()) {
                if (a.type.count() != b.type.count()
                    && a.type.count() > 1 && b.type.count() > 1)
                {
                    throw Exception(cx, stringify(
                        "can't combine lists of different sizes (",
                        a.type.count(), " and ", b.type.count(), ")"));
                }
                return;
            }
            else if (b.type.is_bool32_or_vec()) {
                // TODO: convert a to Bool32 via broadcasting
            }
        }
        else if (a.type.is_bool32_or_vec()) {
            if (b.type.is_bool32_or_vec()) {
                if (a.type.count() != b.type.count()
                    && a.type.count() > 1 && b.type.count() > 1)
                {
                    throw Exception(cx, stringify(
                        "can't combine lists of different sizes (",
                        a.type.count(), " and ", b.type.count(), ")"));
                }
                return;
            }
            if (b.type.is_bool()) {
                // TODO: convert b to Bool32 via broadcasting?
            }
        }
        throw Exception(cx, stringify(
            "arguments must be Bool or Bool32 (got ",
            a.type, " and ", b.type, " instead)"));
    }
};

struct Unary_Num_Prim
{
    typedef double scalar_t;
    static bool unbox(Value a, scalar_t& b, const Context&)
    {
        if (a.is_num()) {
            b = a.to_num_unsafe();
            return true;
        } else
            return false;
    }
    static void sc_check_arg(SC_Value a, const Context& cx)
    {
        if (!a.type.is_num_struc())
            throw Exception(cx, "argument must be a Num, Vec or Mat");
    }
};

struct Binary_Num_Prim : public Unary_Num_Prim
{
    typedef double left_t;
    typedef double right_t;
    static bool unbox_left(Value a, scalar_t& b, const Context& cx)
    {
        return unbox(a, b, cx);
    }
    static bool unbox_right(Value a, scalar_t& b, const Context& cx)
    {
        return unbox(a, b, cx);
    }
    static void sc_check_args(
        SC_Frame& f, SC_Value& a, SC_Value& b, const Context& cx)
    {
        if (!a.type.is_num_struc()) {
            throw Exception(At_Index(0, cx),
                stringify("argument expected to be Num, Vec or Mat; got ",
                    a.type));
        }
        if (!b.type.is_num_struc()) {
            throw Exception(At_Index(1, cx),
                stringify("argument expected to be Num, Vec or Mat; got ",
                    a.type));
        }
        sc_struc_unify(f, a, b, cx);
    }
};

// The left operand is a non-empty list of booleans.
// The right operand is an integer >= 0 and < the size of the left operand.
// (These restrictions on the right operand conform to the definition
// of << and >> in the C/C++/GLSL languages.)
struct Shift_Prim
{
    typedef Shared<const List> left_t;
    typedef double right_t;
    static bool unbox_left(Value a, left_t& b, const Context&)
    {
        b = a.dycast<const List>();
        return b && !b->empty() && b->front().is_bool();
    }
    static bool unbox_right(Value a, right_t& b, const Context&)
    {
        b = a.to_num_or_nan();
        return b == b;
    }
    static void sc_check_args(
        SC_Frame& /*f*/, SC_Value& a, SC_Value& b, const Context& cx)
    {
        if (!a.type.is_bool32_or_vec()) {
            throw Exception(At_Index(0, cx),
                stringify("expected argument of type Bool32, got ", a.type));
        }
        if (b.type != SC_Type::Num()) {
            throw Exception(At_Index(1, cx),
                stringify("expected argument of type Num, got ", b.type));
        }
    }
};

struct Bool32_Prim
{
    static bool unbox_bool32(Value in, unsigned& out, const Context& cx)
    {
        auto li = in.dycast<const List>();
        if (!li || li->size() != 32 || !li->front().is_bool())
            return false;
        out = bool32_to_nat(li, cx);
        return true;
    }
};
struct Unary_Bool32_Prim : public Bool32_Prim
{
    typedef unsigned scalar_t;
    static bool unbox(Value a, scalar_t& b, const Context& cx)
    {
        return unbox_bool32(a, b, cx);
    }
    static void sc_check_arg(SC_Value a, const Context& cx)
    {
        if (!a.type.is_bool32_or_vec())
            throw Exception(cx, "argument must be a Bool32 or list of Bool32");
    }
};
struct Binary_Bool32_Prim : public Bool32_Prim
{
    typedef unsigned left_t;
    typedef unsigned right_t;
    static bool unbox_left(Value a, left_t& b, const Context& cx)
    {
        return unbox_bool32(a, b, At_Index(0, cx));
    }
    static bool unbox_right(Value a, right_t& b, const Context& cx)
    {
        return unbox_bool32(a, b, At_Index(1, cx));
    }
    static void sc_check_arg(SC_Value a, const Context& cx)
    {
        if (!a.type.is_bool32_or_vec())
            throw Exception(cx, "argument must be a Bool32 or list of Bool32");
    }
    static void sc_check_args(
        SC_Frame& /*f*/, SC_Value& a, SC_Value& b, const Context& cx)
    {
        if (!a.type.is_bool32_or_vec()) {
            throw Exception(At_Index(0, cx),
                stringify("expected argument of type Bool32, got ", a.type));
        }
        if (!b.type.is_bool32_or_vec()) {
            throw Exception(At_Index(1, cx),
                stringify("expected argument of type Bool32, got ", b.type));
        }
    }
};

template <class Prim>
struct Binary_Array_Op
{
    // TODO: optimize: move semantics. unique object reuse.
    // TODO: optimize: faster fast path in `op` for number case.

    static Shared<const String> domain_error(Value x, Value y)
    {
        return stringify("[",x,",",y,"]: domain error");
    }

    static Exception domain_error(
        const Context& cx, unsigned i, Value x, Value y)
    {
        if (dynamic_cast<const At_Arg*>(&cx)) {
            return Exception(At_Index(i,cx),
                stringify(i==0?x:y,": domain error"));
        }
        if (auto ap = dynamic_cast<const At_Phrase*>(&cx)) {
            if (auto bin = dynamic_cast<const Binary_Phrase*>(&ap->phrase_))
                return Exception(cx,
                    stringify(x," ",bin->opname()," ",y,": domain error"));
        }
        return Exception(cx, stringify(i==0?x:y,": domain error"));
    }

    static Value
    reduce(const Context& cx, Value zero, Value arg)
    {
        auto list = arg.to<List>(cx);
        Value result = zero;
        for (auto val : *list)
            result = op(cx, result, val);
        return result;
    }
    static SC_Value
    sc_reduce(const Context& cx, Value zero, Operation& argx, SC_Frame& f)
    {
        auto list = dynamic_cast<List_Expr*>(&argx);
        if (list) {
            if (list->empty())
                return sc_eval_const(f, zero, *argx.syntax_);
            auto first = sc_eval_op(f, *list->at(0));
            if (list->size() == 1) {
                Prim::sc_check_arg(first, cx);
                return first;
            }
            for (unsigned i = 1; i < list->size(); ++i) {
                auto second = sc_eval_op(f, *list->at(i));
                Prim::sc_check_args(f, first, second, cx);
                first = Prim::sc_call(f, first, second);
            }
            return first;
        }
        else {
            // Reduce an array value that exists at GPU run time.
            // TODO: For a large 1D array, use a GPU loop and call a function.
            // TODO: Binary_Array_Op::sc_reduce: reduce a matrix.
            // 2D arrays (SC_Type rank 2) are not supported, because you can't
            // generate a rank 1 array at GPU runtime, for now at least.
            // For a single Vec, this inline expansion of the loop is good.
            auto arg = sc_eval_op(f, argx);
            if (arg.type.is_any_vec()) {
                auto first = sc_vec_element(f, arg, 0);
                for (unsigned i = 1; i < arg.type.count(); ++i) {
                    auto second = sc_vec_element(f, arg, 1);
                    Prim::sc_check_args(f, first, second, cx);
                    first = Prim::sc_call(f, first, second);
                }
                return first;
            }
            else {
                throw Exception(cx, "argument is not a vector");
            }
        }
    }

    static Value
    op(const Context& cx, Value x, Value y)
    {
        // fast path: both x and y are scalars
        // remaining cases:
        // - x is a scalar, y is a list
        // - x is a list, y is a scalar
        // - x and y are lists
        // - either x, or y, or both, is reactive
        typename Prim::left_t sx;
        typename Prim::right_t sy;
        if (Prim::unbox_left(x, sx, cx)) {
            if (Prim::unbox_right(y, sy, cx))
                return Prim::call(sx, sy, cx);
            if (y.is_ref()) {
                Ref_Value& ry(y.to_ref_unsafe());
                switch (ry.type_) {
                case Ref_Value::ty_list:
                    return broadcast_right(cx, x, (List&)ry);
                case Ref_Value::ty_reactive:
                    return reactive_op(cx, x, y);
                }
            }
            throw domain_error(cx,1,x,y);
        } else if (x.is_ref()) {
            Ref_Value& rx(x.to_ref_unsafe());
            switch (rx.type_) {
            case Ref_Value::ty_list:
                if (Prim::unbox_right(y, sy, cx))
                    return broadcast_left(cx, (List&)rx, y);
                else if (y.is_ref()) {
                    Ref_Value& ry(y.to_ref_unsafe());
                    switch (ry.type_) {
                    case Ref_Value::ty_list:
                        return element_wise_op(cx, (List&)rx, (List&)ry);
                    case Ref_Value::ty_reactive:
                        return reactive_op(cx, x, y);
                    }
                }
                throw domain_error(cx,1,x,y);
            case Ref_Value::ty_reactive:
                return reactive_op(cx, x, y);
            }
        }
        throw domain_error(cx,0,x,y);
    }
    static SC_Value
    sc_op(const Context& cx, Operation& argx, SC_Frame& f)
    {
        auto list = dynamic_cast<List_Expr*>(&argx);
        if (list && list->size() == 2) {
            auto first = sc_eval_op(f, *list->at(0));
            auto second = sc_eval_op(f, *list->at(1));
            Prim::sc_check_args(f, first, second, cx);
            return Prim::sc_call(f, first, second);
        }
        // TODO: Binary_Array_Op::sc_op: accept a 2-vector, 2-array or mat2.
        throw Exception(cx, "expected a list of size 2");
    }
    static SC_Value
    sc_call(SC_Frame& f, Operation& ax, Operation& ay, Shared<const Phrase> ph)
    {
        auto x = sc_eval_op(f, ax);
        auto y = sc_eval_op(f, ay);
        Prim::sc_check_args(f, x, y, At_SC_Phrase(ph, f));
        return Prim::sc_call(f, x, y);
    }

    static Value
    broadcast_left(const Context& cx, List& xlist, Value y)
    {
        Shared<List> result = List::make(xlist.size());
        for (unsigned i = 0; i < xlist.size(); ++i)
            (*result)[i] = op(cx, xlist[i], y);
        return {result};
    }

    static Value
    broadcast_right(const Context& cx, Value x, List& ylist)
    {
        Shared<List> result = List::make(ylist.size());
        for (unsigned i = 0; i < ylist.size(); ++i)
            (*result)[i] = op(cx, x, ylist[i]);
        return {result};
    }

    static Value
    element_wise_op(const Context& cx, List& xs, List& ys)
    {
        if (xs.size() != ys.size())
            throw Exception(cx, stringify(
                "mismatched list sizes (",
                xs.size(),",",ys.size(),") in array operation"));
        Shared<List> result = List::make(xs.size());
        for (unsigned i = 0; i < xs.size(); ++i)
            (*result)[i] = op(cx, xs[i], ys[i]);
        return {result};
    }

    static Value
    reactive_op(const Context& cx, Value x, Value y)
    {
    /*
        // At least one of x and y is reactive. Cases:
        //   list * list
        //   scalar * list
        //   list * scalar
        //   scalar * scalar;
        // where list and scalar are either constants or reactive.
        // Need to compute the result SC_Type now, and thus I need to look at
        // these cases right here.
        Shared<const Reactive_Value> xr, yr;
        SC_Type xt, yt;
        f.get_expr(x, xr, xt);
        f.get_expr(y, yr, yt);
    //
        if (x.is_num()) {
            auto yre = y.dycast<Reactive_Value>();
            if (yre && yre->sctype_ == SC_Type::Num()) {
                auto& syn = f.cx.syntax();
                return {make<Reactive_Expression>(
                    SC_Type::Num(),
                    f.make_expr(
                        make<Constant>(share(syn), x),
                        yre->expr(syn)),
                    f.cx)};
            }
        }
    */
        throw Exception(cx, domain_error(x, y));
    }
};

template <class Scalar_Op>
struct Unary_Numeric_Array_Op
{
    // TODO: optimize: move semantics. unique object reuse.

    static Value
    op(const Scalar_Op& f, Value x)
    {
        double r = f.call(x.to_num_or_nan());
        if (r == r)
            return {r};
        if (auto xlist = x.dycast<List>())
            return {element_wise_op(f, xlist)};
        auto xre = x.dycast<Reactive_Value>();
        if (xre && xre->sctype_ == SC_Type::Num()) {
            return {make<Reactive_Expression>(
                SC_Type::Num(),
                f.make_expr(xre->expr(f.cx.syntax())),
                f.cx)};
        }
        throw Exception(f.cx,
            stringify(f.callstr(x),": domain error"));
    }

    static Shared<List>
    element_wise_op(const Scalar_Op& f, Shared<List> xs)
    {
        Shared<List> result = List::make(xs->size());
        for (unsigned i = 0; i < xs->size(); ++i)
            (*result)[i] = op(f, (*xs)[i]);
        return result;
    }
};

template <class Prim>
struct Unary_Array_Op
{
    // TODO: optimize: move semantics. unique object reuse.

    static Value
    op(const Context& cx, Value x)
    {
        typename Prim::scalar_t sx;
        if (Prim::unbox(x, sx, cx)) {
            return Prim::call(sx, cx);
        } else if (x.is_ref()) {
            Ref_Value& rx(x.to_ref_unsafe());
            switch (rx.type_) {
            case Ref_Value::ty_list:
                return element_wise_op(cx, (List&)rx);
            case Ref_Value::ty_reactive:
                return reactive_op(cx, x);
            }
        }
        throw Exception(cx, domain_error(x));
    }
    static SC_Value
    sc_op(const Context& cx, Operation& argx, SC_Frame& f)
    {
        // TODO: add array support
        auto a = sc_eval_op(f, argx);
        Prim::sc_check_arg(a, cx);
        return Prim::sc_call(f, a);
    }

    static Value
    element_wise_op(const Context& cx, List& xs)
    {
        Shared<List> result = List::make(xs.size());
        for (unsigned i = 0; i < xs.size(); ++i)
            (*result)[i] = op(cx, xs[i]);
        return {result};
    }

    static Value
    reactive_op(const Context& cx, Value x)
    {
        throw Exception(cx, domain_error(x));
    }

    static Shared<const String> domain_error(Value x)
    {
        return stringify(x, ": domain error");
    }
};

} // namespace
#endif // header guard
