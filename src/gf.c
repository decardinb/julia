/*
  Generic Functions
  . method table and lookup
  . GF constructor, add_method
  . dispatch
  . static parameter inference
  . method specialization, invoking type inference
*/
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "julia.h"
#include "builtin_proto.h"

static jl_methtable_t *new_method_table(void)
{
    jl_methtable_t *mt = (jl_methtable_t*)allocobj(sizeof(jl_methtable_t));
    mt->type = (jl_type_t*)jl_methtable_type;
    mt->defs = NULL;
    mt->cache = NULL;
    mt->cache_arg1 = NULL;
    mt->cache_targ = NULL;
    mt->max_args = jl_box_long(0);
#ifdef JL_GF_PROFILE
    mt->ncalls = 0;
#endif
    return mt;
}

static int cache_match_by_type(jl_value_t **types, size_t n, jl_tuple_t *sig,
                               int va)
{
    if (!va && n > sig->length)
        return 0;
    if (sig->length > n) {
        if (!(n == sig->length-1 && va))
            return 0;
    }
    size_t i;
    for(i=0; i < n; i++) {
        jl_value_t *decl = jl_tupleref(sig, i);
        if (i == sig->length-1) {
            if (va) {
                jl_value_t *t = jl_tparam0(decl);
                for(; i < n; i++) {
                    if (!jl_subtype(types[i], t, 0))
                        return 0;
                }
                return 1;
            }
        }
        jl_value_t *a = types[i];
        if (jl_is_tuple(decl)) {
            // tuples don't have to match exactly, to avoid caching
            // signatures for tuples of every length
            if (!jl_subtype(a, decl, 0))
                return 0;
        }
        else if (jl_is_tag_type(a) && jl_is_tag_type(decl) &&
                 ((jl_tag_type_t*)decl)->name == jl_type_type->name &&
                 ((jl_tag_type_t*)a   )->name == jl_type_type->name) {
            if (jl_tparam0(decl) == (jl_value_t*)jl_typetype_tvar) {
                // in the case of Type{T}, the types don't have
                // to match exactly either. this is cached as Type{T}.
                // analogous to the situation with tuples.
            }
            else {
                if (!jl_types_equal(jl_tparam0(a), jl_tparam0(decl))) {
                    return 0;
                }
            }
        }
        else if (decl == (jl_value_t*)jl_any_type) {
        }
        else {
            if (!jl_types_equal(a, decl))
                return 0;
        }
    }
    return 1;
}

static inline int cache_match(jl_value_t **args, size_t n, jl_tuple_t *sig,
                              int va)
{
    if (sig->length > n) {
        if (n != sig->length-1)
            return 0;
    }
    size_t i;
    for(i=0; i < n; i++) {
        jl_value_t *decl = jl_tupleref(sig, i);
        if (i == sig->length-1) {
            if (va) {
                jl_value_t *t = jl_tparam0(decl);
                for(; i < n; i++) {
                    if (!jl_subtype(args[i], t, 1))
                        return 0;
                }
                return 1;
            }
        }
        jl_value_t *a = args[i];
        if (jl_is_tuple(decl)) {
            // tuples don't have to match exactly, to avoid caching
            // signatures for tuples of every length
            if (!jl_is_tuple(a) || !jl_subtype(a, decl, 1))
                return 0;
        }
        else if (jl_is_type_type(decl) &&
                 jl_is_nontuple_type(a)) {   //***
            if (jl_tparam0(decl) == (jl_value_t*)jl_typetype_tvar) {
                // in the case of Type{T}, the types don't have
                // to match exactly either. this is cached as Type{T}.
                // analogous to the situation with tuples.
            }
            else {
                if (a!=jl_tparam0(decl) && !jl_types_equal(a,jl_tparam0(decl)))
                    return 0;
            }
        }
        else if (decl == (jl_value_t*)jl_any_type) {
        }
        else {
            /*
              we know there are only concrete types here, and types are
              hash-consed, so pointer comparison should work.
            */
            if ((jl_value_t*)jl_typeof(a) != decl)
                return 0;
        }
    }
    return 1;
}

/*
  Method caches are divided into three parts: one for signatures where
  the first argument is a singleton kind (Type{Foo}), one indexed by the
  UID of the first argument's type in normal cases, and a fallback
  table of everything else.
*/
static jl_function_t *jl_method_table_assoc_exact_by_type(jl_methtable_t *mt,
                                                          jl_tuple_t *types)
{
    jl_methlist_t *ml = NULL;
    if (types->length > 0) {
        jl_value_t *ty = jl_t0(types);
        uptrint_t uid;
        if (jl_is_type_type(ty)) {
            jl_value_t *a0 = jl_tparam0(ty);
            jl_value_t *tty = (jl_value_t*)jl_typeof(a0);
            if ((tty == (jl_value_t*)jl_struct_kind && (uid = ((jl_struct_type_t*)a0)->uid)) ||
                (tty == (jl_value_t*)jl_bits_kind   && (uid = ((jl_bits_type_t*)a0)->uid))) {
                if (mt->cache_targ &&
                    uid < jl_array_len(mt->cache_targ)) {
                    ml = (jl_methlist_t*)jl_cellref(mt->cache_targ, uid);
                    if (ml)
                        goto mt_assoc_bt_lkup;
                }
            }
        }
        if ((jl_is_struct_type(ty) && (uid = ((jl_struct_type_t*)ty)->uid)) ||
            (jl_is_bits_type(ty)   && (uid = ((jl_bits_type_t*)ty)->uid))) {
            if (mt->cache_arg1 && uid < jl_array_len(mt->cache_arg1)) {
                ml = (jl_methlist_t*)jl_cellref(mt->cache_arg1, uid);
            }
        }
    }
    if (ml == NULL)
        ml = mt->cache;
 mt_assoc_bt_lkup:
    while (ml != NULL) {
        if (cache_match_by_type(&jl_tupleref(types,0), types->length,
                                (jl_tuple_t*)ml->sig, ml->va==jl_true)) {
            return ml->func;
        }
        ml = ml->next;
    }
    return NULL;
}

static jl_function_t *jl_method_table_assoc_exact(jl_methtable_t *mt,
                                                  jl_value_t **args, size_t n)
{
    jl_methlist_t *ml = NULL;
    if (n > 0) {
        jl_value_t *a0 = args[0];
        jl_value_t *ty = (jl_value_t*)jl_typeof(a0);
        uptrint_t uid;
        if ((ty == (jl_value_t*)jl_struct_kind && (uid = ((jl_struct_type_t*)a0)->uid)) ||
            (ty == (jl_value_t*)jl_bits_kind   && (uid = ((jl_bits_type_t*)a0)->uid))) {
            if (mt->cache_targ &&
                uid < jl_array_len(mt->cache_targ)) {
                ml = (jl_methlist_t*)jl_cellref(mt->cache_targ, uid);
                if (ml)
                    goto mt_assoc_lkup;
            }
        }
        if ((jl_is_struct_type(ty) && (uid = ((jl_struct_type_t*)ty)->uid)) ||
            (jl_is_bits_type(ty)   && (uid = ((jl_bits_type_t*)ty)->uid))) {
            if (mt->cache_arg1 &&
                uid < jl_array_len(mt->cache_arg1)) {
                ml = (jl_methlist_t*)jl_cellref(mt->cache_arg1, uid);
                if (ml) {
                    if (ml->next==NULL && n==1 && ml->sig->length==1)
                        return ml->func;
                    if (n==2) {
                        // some manually-unrolled common special cases
                        jl_value_t *a1 = args[1];
                        jl_methlist_t *mn = ml;
                        if (mn->sig->length==2 &&
                            jl_tupleref(mn->sig,1)==(jl_value_t*)jl_typeof(a1))
                            return mn->func;
                        mn = mn->next;
                        if (mn && mn->sig->length==2 &&
                            jl_tupleref(mn->sig,1)==(jl_value_t*)jl_typeof(a1))
                            return mn->func;
                    }
                }
            }
        }
    }
    if (ml == NULL)
        ml = mt->cache;
 mt_assoc_lkup:
    while (ml != NULL) {
        if (((jl_tuple_t*)ml->sig)->length == n || ml->va==jl_true) {
            if (cache_match(args, n, (jl_tuple_t*)ml->sig, ml->va==jl_true)) {
                return ml->func;
            }
        }
        ml = ml->next;
    }
    return NULL;
}

// return a new lambda-info that has some extra static parameters
// merged in.
jl_lambda_info_t *jl_add_static_parameters(jl_lambda_info_t *l, jl_tuple_t *sp)
{
    JL_GC_PUSH(&sp);
    if (l->sparams->length > 0)
        sp = jl_tuple_append(sp, l->sparams);
    jl_lambda_info_t *nli = jl_new_lambda_info(l->ast, sp);
    nli->name = l->name;
    nli->fptr = l->fptr;
    nli->module = l->module;
    nli->file = l->file;
    nli->line = l->line;
    JL_GC_POP();
    return nli;
}

JL_CALLABLE(jl_trampoline);

jl_function_t *jl_instantiate_method(jl_function_t *f, jl_tuple_t *sp)
{
    if (f->linfo == NULL)
        return f;
    jl_function_t *nf = jl_new_closure(f->fptr, f->env, NULL);
    JL_GC_PUSH(&nf);
    nf->linfo = jl_add_static_parameters(f->linfo, sp);
    JL_GC_POP();
    return nf;
}

// make a new method that calls the generated code from the given linfo
jl_function_t *jl_reinstantiate_method(jl_function_t *f, jl_lambda_info_t *li)
{
    return jl_new_closure(NULL, f->env, li);
}

static
jl_methlist_t *jl_method_list_insert(jl_methlist_t **pml, jl_tuple_t *type,
                                     jl_function_t *method, jl_tuple_t *tvars,
                                     int check_amb);

static
jl_function_t *jl_method_cache_insert(jl_methtable_t *mt, jl_tuple_t *type,
                                      jl_function_t *method)
{
    jl_methlist_t **pml = &mt->cache;
    if (type->length > 0) {
        jl_value_t *t0 = jl_t0(type);
        uptrint_t uid=0;
        // if t0 != jl_typetype_type and the argument is Type{...}, this
        // method has specializations for singleton kinds and we use
        // the table indexed for that purpose.
        if (t0 != (jl_value_t*)jl_typetype_type && jl_is_type_type(t0)) {
            jl_value_t *a0 = jl_tparam0(t0);
            if (jl_is_struct_type(a0))
                uid = ((jl_struct_type_t*)a0)->uid;
            else if (jl_is_bits_type(a0))
                uid = ((jl_bits_type_t*)a0)->uid;
            if (uid > 0) {
                if (mt->cache_targ == NULL)
                    mt->cache_targ = jl_alloc_cell_1d(0);
                if (uid >= jl_array_len(mt->cache_targ)) {
                    jl_array_grow_end(mt->cache_targ, uid+4-jl_array_len(mt->cache_targ));
                }
                pml = (jl_methlist_t**)&jl_cellref(mt->cache_targ, uid);
                goto ml_do_insert;
            }
        }
        if (jl_is_struct_type(t0))
            uid = ((jl_struct_type_t*)t0)->uid;
        else if (jl_is_bits_type(t0))
            uid = ((jl_bits_type_t*)t0)->uid;
        if (uid > 0) {
            if (mt->cache_arg1 == NULL)
                mt->cache_arg1 = jl_alloc_cell_1d(0);
            if (uid >= jl_array_len(mt->cache_arg1)) {
                jl_array_grow_end(mt->cache_arg1, uid+4-jl_array_len(mt->cache_arg1));
            }
            pml = (jl_methlist_t**)&jl_cellref(mt->cache_arg1, uid);
        }
    }
 ml_do_insert:
    return jl_method_list_insert(pml, type, method, jl_null, 0)->func;
}

extern jl_function_t *jl_typeinf_func;
#define ENABLE_INFERENCE
//#define TRACE_INFERENCE

#ifdef TRACE_INFERENCE
static char *type_summary(jl_value_t *t);
static void print_sig(jl_tuple_t *type)
{
    size_t i;
    for(i=0; i < type->length; i++) {
        if (i > 0) ios_printf(ios_stderr, ", ");
        jl_value_t *v = jl_tupleref(type,i);
        if (jl_is_tuple(v)) {
            ios_putc('(', ios_stderr);
            print_sig((jl_tuple_t*)v);
            ios_putc(')', ios_stderr);
        }
        else {
            ios_printf(ios_stderr, "%s", type_summary(v));
        }
    }
}
#endif

static jl_value_t *nth_slot_type(jl_tuple_t *sig, size_t i)
{
    size_t len = sig->length;
    if (len == 0)
        return NULL;
    if (i < len-1)
        return jl_tupleref(sig, i);
    if (jl_is_seq_type(jl_tupleref(sig,len-1))) {
        return jl_tparam0(jl_tupleref(sig,len-1));
    }
    if (i == len-1)
        return jl_tupleref(sig, i);
    return NULL;
}

static int very_general_type(jl_value_t *t)
{
    return (t && (t==(jl_value_t*)jl_any_type ||
                  (jl_is_typevar(t) &&
                   ((jl_tvar_t*)t)->ub==(jl_value_t*)jl_any_type)));
}

static jl_value_t *ml_matches(jl_methlist_t *ml, jl_value_t *type,
                              jl_sym_t *name, int lim);

/*
  run type inference on lambda "li" in-place, for given argument types.
  "def" is the original method definition of which this is an instance;
  can be equal to "li" if not applicable.
*/
int jl_in_inference = 0;
void jl_type_infer(jl_lambda_info_t *li, jl_tuple_t *argtypes,
                   jl_lambda_info_t *def)
{
    int last_ii = jl_in_inference;
    jl_in_inference = 1;
    if (jl_typeinf_func != NULL) {
        // TODO: this should be done right before code gen, so if it is
        // interrupted we can try again the next time the function is
        // called
        assert(li->inInference == 0);
        li->inInference = 1;
        jl_value_t *fargs[4];
        fargs[0] = (jl_value_t*)li;
        fargs[1] = (jl_value_t*)argtypes;
        fargs[2] = (jl_value_t*)jl_null;
        fargs[3] = (jl_value_t*)def;
#ifdef TRACE_INFERENCE
        ios_printf(ios_stderr,"inference on %s(", li->name->name);
        print_sig(argtypes);
        ios_printf(ios_stderr, ")\n");
#endif
#ifdef ENABLE_INFERENCE
        jl_value_t *newast = jl_apply(jl_typeinf_func, fargs, 4);
        li->ast = jl_tupleref(newast, 0);
        li->inferred = jl_true;
#endif
        li->inInference = 0;
    }
    jl_in_inference = last_ii;
}

static int tuple_all_Any(jl_tuple_t *t)
{
    int i;
    for(i=0; i < t->length; i++) {
        if (jl_tupleref(t,i) != (jl_value_t*)jl_any_type)
            return 0;
    }
    return 1;
}

static jl_function_t *cache_method(jl_methtable_t *mt, jl_tuple_t *type,
                                   jl_function_t *method, jl_tuple_t *decl,
                                   jl_tuple_t *sparams)
{
    size_t i;
    int need_dummy_entries = 0;
    jl_value_t *temp=NULL;
    jl_function_t *newmeth=NULL;
    JL_GC_PUSH(&type, &temp, &newmeth);

    for (i=0; i < type->length; i++) {
        jl_value_t *elt = jl_tupleref(type,i);
        int set_to_any = 0;
        if (nth_slot_type(decl,i) == jl_ANY_flag) {
            // don't specialize on slots marked ANY
            temp = jl_tupleref(type, i);
            jl_tupleset(type, i, (jl_value_t*)jl_any_type);
            int nintr=0;
            jl_methlist_t *curr = mt->defs;
            // if this method is the only match even with the current slot
            // set to Any, then it is safe to cache it that way.
            while (curr != NULL && curr->func!=method) {
                if (jl_type_intersection((jl_value_t*)curr->sig,
                                         (jl_value_t*)type) !=
                    (jl_value_t*)jl_bottom_type) {
                    nintr++;
                    break;
                }
                curr = curr->next;
            }
            if (nintr) {
                // TODO: even if different specializations of this slot need
                // separate cache entries, have them share code.
                jl_tupleset(type, i, temp);
            }
            else {
                set_to_any = 1;
            }
        }
        if (set_to_any) {
        }
        else if (jl_is_tuple(elt)) {
            /*
              don't cache tuple type exactly; just remember that it was
              a tuple, unless the declaration asks for something more
              specific. determined with a type intersection.
            */
            int might_need_dummy=0;
            temp = jl_tupleref(type, i);
            if (i < decl->length) {
                jl_value_t *declt = jl_tupleref(decl,i);
                // for T..., intersect with T
                if (jl_is_seq_type(declt))
                    declt = jl_tparam0(declt);
                if (declt == (jl_value_t*)jl_tuple_type ||
                    jl_subtype((jl_value_t*)jl_tuple_type, declt, 0)) {
                    // don't specialize args that matched (Any...) or Any
                    jl_tupleset(type, i, (jl_value_t*)jl_tuple_type);
                    might_need_dummy = 1;
                }
                else {
                    declt = jl_type_intersection(declt,
                                                 (jl_value_t*)jl_tuple_type);
                    if (((jl_tuple_t*)elt)->length > 3 ||
                        tuple_all_Any((jl_tuple_t*)declt)) {
                        jl_tupleset(type, i, declt);
                        might_need_dummy = 1;
                    }
                }
            }
            else {
                jl_tupleset(type, i, (jl_value_t*)jl_tuple_type);
                might_need_dummy = 1;
            }
            assert(jl_tupleref(type,i) != (jl_value_t*)jl_bottom_type);
            if (might_need_dummy) {
                jl_methlist_t *curr = mt->defs;
                // can't generalize type if there's an overlapping definition
                // with typevars
                while (curr != NULL && curr->func!=method) {
                    if (curr->tvars!=jl_null &&
                        jl_type_intersection((jl_value_t*)curr->sig,
                                             (jl_value_t*)type) !=
                        (jl_value_t*)jl_bottom_type) {
                        jl_tupleset(type, i, temp);
                        might_need_dummy = 0;
                        break;
                    }
                    curr = curr->next;
                }
            }
            if (might_need_dummy) {
                jl_methlist_t *curr = mt->defs;
                while (curr != NULL && curr->func!=method) {
                    jl_tuple_t *sig = curr->sig;
                    if (sig->length > i &&
                        jl_is_tuple(jl_tupleref(sig,i))) {
                        need_dummy_entries = 1;
                        break;
                    }
                    curr = curr->next;
                }
            }
        }
        else if (jl_is_type_type(elt) && jl_is_type_type(jl_tparam0(elt))) {
            /*
              actual argument was Type{...}, we computed its type as
              Type{Type{...}}. we must avoid unbounded nesting here, so
              cache the signature as Type{T}, unless something more
              specific like Type{Type{Int32}} was actually declared.
              this can be determined using a type intersection.
            */
            if (i < decl->length) {
                jl_value_t *declt = jl_tupleref(decl,i);
                // for T..., intersect with T
                if (jl_is_seq_type(declt))
                    declt = jl_tparam0(declt);
                jl_tupleset(type, i,
                            jl_type_intersection(declt, (jl_value_t*)jl_typetype_type));
            }
            else {
                jl_tupleset(type, i, (jl_value_t*)jl_typetype_type);
            }
            assert(jl_tupleref(type,i) != (jl_value_t*)jl_bottom_type);
        }
        else if (jl_is_type_type(elt) &&
                 very_general_type(nth_slot_type(decl,i))) {
            /*
              here's a fairly complex heuristic: if this argument slot's
              declared type is Any, and no definition overlaps with Type
              for this slot, then don't specialize for every Type that
              might be passed.
              Since every type x has its own type Type{x}, this would be
              excessive specialization for an Any slot.
            */
            int ok=1;
            jl_methlist_t *curr = mt->defs;
            while (curr != NULL) {
                jl_value_t *slottype = nth_slot_type(curr->sig, i);
                if (slottype &&
                    !very_general_type(slottype) &&
                    jl_type_intersection(slottype,
                                         (jl_value_t*)jl_type_type) !=
                    (jl_value_t*)jl_bottom_type) {
                    ok=0;
                    break;
                }
                curr = curr->next;
            }
            if (ok) {
                jl_tupleset(type, i, (jl_value_t*)jl_typetype_type);
            }
        }
    }

    // for varargs methods, only specialize up to max_args.
    // in general, here we want to find the biggest type that's not a
    // supertype of any other method signatures. so far we are conservative
    // and the types we find should be bigger.
    if (type->length > jl_unbox_long(mt->max_args) &&
        jl_is_seq_type(jl_tupleref(decl,decl->length-1))) {
        size_t nspec = jl_unbox_long(mt->max_args)+2;
        jl_tuple_t *limited = jl_alloc_tuple(nspec);
        for(i=0; i < nspec-1; i++) {
            jl_tupleset(limited, i, jl_tupleref(type, i));
        }
        jl_value_t *lasttype = jl_tupleref(type,i-1);
        // if all subsequent arguments are subtypes of lasttype, specialize
        // on that instead of decl. for example, if decl is
        // (Any...)
        // and type is
        // (Symbol, Symbol, Symbol)
        // then specialize as (Symbol...), but if type is
        // (Symbol, Int32, Expr)
        // then specialize as (Any...)
        size_t j = i;
        int all_are_subtypes=1;
        for(; j < type->length; j++) {
            if (!jl_subtype(jl_tupleref(type,j), lasttype, 0)) {
                all_are_subtypes = 0;
                break;
            }
        }
        type = limited;
        if (all_are_subtypes) {
            // avoid Type{Type{...}...}...
            if (jl_is_type_type(lasttype))
                lasttype = (jl_value_t*)jl_type_type;
            temp = (jl_value_t*)jl_tuple1(lasttype);
            jl_tupleset(type, i, jl_apply_type((jl_value_t*)jl_seq_type,
                                               (jl_tuple_t*)temp));
        }
        else {
            jl_value_t *lastdeclt = jl_tupleref(decl,decl->length-1);
            if (sparams->length > 0) {
                lastdeclt = (jl_value_t*)
                    jl_instantiate_type_with((jl_type_t*)lastdeclt,
                                             sparams->data,
                                             sparams->length/2);
            }
            jl_tupleset(type, i, lastdeclt);
        }
        // now there is a problem: the computed signature is more
        // general than just the given arguments, so it might conflict
        // with another definition that doesn't have cache instances yet.
        // to fix this, we insert dummy cache entries for all intersections
        // of this signature and definitions. those dummy entries will
        // supersede this one in conflicted cases, alerting us that there
        // should actually be a cache miss.
        need_dummy_entries = 1;
    }

    if (need_dummy_entries) {
        temp = ml_matches(mt->defs, (jl_value_t*)type, lambda_sym, -1);
        for(i=0; i < jl_array_len(temp); i++) {
            jl_value_t *m = jl_cellref(temp, i);
            if (jl_tupleref(m,2) != (jl_value_t*)method->linfo) {
                jl_method_cache_insert(mt, (jl_tuple_t*)jl_tupleref(m, 0),
                                       NULL);
            }
        }
    }

    // here we infer types and specialize the method
    /*
    if (sparams==jl_null)
        newmeth = method;
    else
    */
    jl_array_t *lilist=NULL;
    jl_lambda_info_t *li=NULL;
    if (method->linfo && method->linfo->specializations!=NULL) {
        // reuse code already generated for this combination of lambda and
        // arguments types. this happens for inner generic functions where
        // a new closure is generated on each call to the enclosing function.
        lilist = method->linfo->specializations;
        int k;
        for(k=0; k < lilist->length; k++) {
            li = (jl_lambda_info_t*)jl_cellref(lilist, k);
            if (jl_types_equal(li->specTypes, (jl_value_t*)type))
                break;
        }
        if (k == lilist->length) lilist=NULL;
    }
    if (lilist != NULL && !li->inInference) {
        assert(li);
        newmeth = jl_reinstantiate_method(method, li);
        (void)jl_method_cache_insert(mt, type, newmeth);
        JL_GC_POP();
        return newmeth;
    }
    else {
        newmeth = jl_instantiate_method(method, sparams);
    }
    /*
      if "method" itself can ever be compiled, for example for use as
      an unspecialized method (see below), then newmeth->fptr might point
      to some slow compiled code instead of jl_trampoline, meaning our
      type-inferred code would never get compiled. this can be fixed with
      the commented-out snippet below.
    */
    assert(!(newmeth->linfo && newmeth->linfo->ast) ||
           newmeth->fptr == &jl_trampoline);
    /*
    if (newmeth->linfo&&newmeth->linfo->ast&&newmeth->fptr!=&jl_trampoline) {
        newmeth->fptr = &jl_trampoline;
    }
    */

    (void)jl_method_cache_insert(mt, type, newmeth);

    if (newmeth->linfo != NULL && newmeth->linfo->sparams == jl_null) {
        // when there are no static parameters, one unspecialized version
        // of a function can be shared among all cached specializations.
        if (method->linfo->unspecialized == NULL) {
            method->linfo->unspecialized =
                jl_instantiate_method(method, jl_null);
        }
        newmeth->linfo->unspecialized = method->linfo->unspecialized;
    }

    if (newmeth->linfo != NULL && newmeth->linfo->ast != NULL) {
        newmeth->linfo->specTypes = (jl_value_t*)type;
        jl_array_t *spe = method->linfo->specializations;
        if (spe == NULL) {
            spe = jl_alloc_cell_1d(1);
            jl_cellset(spe, 0, newmeth->linfo);
        }
        else {
            jl_cell_1d_push(spe, (jl_value_t*)newmeth->linfo);
        }
        method->linfo->specializations = spe;
        jl_type_infer(newmeth->linfo, type, method->linfo);
    }
    JL_GC_POP();
    return newmeth;
}

static jl_function_t *jl_mt_assoc_by_type(jl_methtable_t *mt, jl_tuple_t *tt, int cache)
{
    jl_methlist_t *m = mt->defs;
    size_t nargs = tt->length;
    size_t i;
    jl_value_t *env = jl_false;

    while (m != NULL) {
        if (m->tvars!=jl_null) {
            env = jl_type_match((jl_value_t*)tt, (jl_value_t*)m->sig);
            if (env != jl_false) {
                // parametric methods only match if all typevars are matched by
                // non-typevars.
                for(i=1; i < ((jl_tuple_t*)env)->length; i+=2) {
                    if (jl_is_typevar(jl_tupleref(env,i)))
                        break;
                }
                if (i >= ((jl_tuple_t*)env)->length)
                    break;
                env = jl_false;
            }
        }
        else if (jl_tuple_subtype(&jl_tupleref(tt,0), nargs,
                                  &jl_tupleref(m->sig,0),
                                  ((jl_tuple_t*)m->sig)->length, 0, 0)) {
            break;
        }
        m = m->next;
    }

    if (env == (jl_value_t*)jl_false) {
        if (m != NULL) {
            if (!cache) {
                return m->func;
            }
            return cache_method(mt, tt, m->func, (jl_tuple_t*)m->sig, jl_null);
        }
        return NULL;
    }

    jl_tuple_t *newsig=NULL;
    JL_GC_PUSH(&env, &newsig);

    assert(jl_is_tuple(env));
    jl_tuple_t *tpenv = (jl_tuple_t*)env;
    // don't bother computing this if no arguments are tuples
    for(i=0; i < tt->length; i++) {
        if (jl_is_tuple(jl_tupleref(tt,i)))
            break;
    }
    if (i < tt->length) {
        newsig = (jl_tuple_t*)jl_instantiate_type_with((jl_type_t*)m->sig,
                                                       &jl_tupleref(tpenv,0),
                                                       tpenv->length/2);
    }
    else {
        newsig = (jl_tuple_t*)m->sig;
    }
    assert(jl_is_tuple(newsig));
    jl_function_t *nf;
    if (!cache)
        nf = m->func;
    else
        nf = cache_method(mt, tt, m->func, newsig, tpenv);
    JL_GC_POP();
    return nf;
}

jl_tag_type_t *jl_wrap_Type(jl_value_t *t);

static int sigs_eq(jl_value_t *a, jl_value_t *b)
{
    if (jl_has_typevars(a) || jl_has_typevars(b)) {
        return jl_types_equal_generic(a,b);
    }
    return jl_types_equal(a, b);
}

int jl_args_morespecific(jl_value_t *a, jl_value_t *b)
{
    int msp = jl_type_morespecific(a,b,0);
    if (jl_has_typevars(b)) {
        if (jl_type_match_morespecific(a,b) == (jl_value_t*)jl_false) {
            if (jl_has_typevars(a)) {
                return 0;
            }
            return msp;
        }
        if (jl_has_typevars(a)) {
            if (jl_type_match_morespecific(b,a) == (jl_value_t*)jl_false) {
                return 1;
            }
        }
        int nmsp = jl_type_morespecific(b,a,0);
        if (nmsp == msp)
            return 0;
    }
    if (jl_has_typevars((jl_value_t*)a)) {
        int nmsp = jl_type_morespecific(b,a,0);
        if (nmsp && msp)
            return 1;
        if (jl_type_match_morespecific(b,a) != (jl_value_t*)jl_false) {
            return 0;
        }
    }
    return msp;
}

static int is_va_tuple(jl_tuple_t *t)
{
    return (t->length>0 && jl_is_seq_type(jl_tupleref(t,t->length-1)));
}

/*
  warn about ambiguous method priorities
  
  the relative priority of A and B is ambiguous if
  !subtype(A,B) && !subtype(B,A) && no corresponding tuple
  elements are disjoint.
  
  for example, (AbstractArray, AbstractMatrix) and (AbstractMatrix, AbstractArray) are ambiguous.
  however, (AbstractArray, AbstractMatrix, Foo) and (AbstractMatrix, AbstractArray, Bar) are fine
  since Foo and Bar are disjoint, so there would be no confusion over
  which one to call.
  
  There is also this kind of ambiguity: foo{T,S}(T, S) vs. foo(Any,Any)
  In this case jl_types_equal() is true, but one is jl_type_morespecific
  or jl_type_match_morespecific than the other.
  To check this, jl_types_equal_generic needs to be more sophisticated
  so (T,T) is not equivalent to (Any,Any). (TODO)
*/
static void check_ambiguous(jl_methlist_t *ml, jl_tuple_t *type,
                            jl_tuple_t *sig, jl_sym_t *fname)
{
    // we know !jl_args_morespecific(type, sig)
    if ((type->length==sig->length ||
         (type->length==sig->length+1 && is_va_tuple(type)) ||
         (type->length+1==sig->length && is_va_tuple(sig))) &&
        !jl_args_morespecific((jl_value_t*)sig, (jl_value_t*)type)) {
        jl_value_t *isect = jl_type_intersection((jl_value_t*)type,
                                                 (jl_value_t*)sig);
        if (isect == (jl_value_t*)jl_bottom_type)
            return;
        JL_GC_PUSH(&isect);
        jl_methlist_t *l = ml;
        while (l != NULL) {
            if (sigs_eq(isect, (jl_value_t*)l->sig))
                goto done_chk_amb;  // ok, intersection is covered
            l = l->next;
        }
        char *n = fname->name;
        jl_value_t *errstream = jl_get_global(jl_base_module,
                                              jl_symbol("stderr_stream"));
        JL_TRY {
            if (errstream)
                jl_set_current_output_stream_obj(errstream);
            ios_t *s = jl_current_output_stream();
            ios_printf(s, "Warning: New definition %s", n);
            jl_show((jl_value_t*)type);
            ios_printf(s, " is ambiguous with %s", n);
            jl_show((jl_value_t*)sig);
            ios_printf(s, ".\n         Make sure %s", n);
            jl_show(isect);
            ios_printf(s, " is defined first.\n");
        }
        JL_CATCH {
            jl_raise(jl_exception_in_transit);
        }
    done_chk_amb:
        JL_GC_POP();
    }
}

static int has_unions(jl_tuple_t *type)
{
    int i;
    for(i=0; i < type->length; i++) {
        jl_value_t *t = jl_tupleref(type,i);
        if (jl_is_union_type(t) ||
            (jl_is_seq_type(t) && jl_is_union_type(jl_tparam0(t))))
            return 1;
    }
    return 0;
}

static
jl_methlist_t *jl_method_list_insert(jl_methlist_t **pml, jl_tuple_t *type,
                                     jl_function_t *method, jl_tuple_t *tvars,
                                     int check_amb)
{
    jl_methlist_t *l, **pl;

    assert(jl_is_tuple(type));
    l = *pml;
    while (l != NULL) {
        if (((l->tvars==jl_null) == (tvars==jl_null)) &&
            sigs_eq((jl_value_t*)type, (jl_value_t*)l->sig)) {
            // method overwritten
            JL_SIGATOMIC_BEGIN();
            l->sig = type;
            l->tvars = tvars;
            l->va = (type->length > 0 &&
                     jl_is_seq_type(jl_tupleref(type,type->length-1))) ?
                jl_true : jl_false;
            l->invokes = NULL;
            l->func = method;
            JL_SIGATOMIC_END();
            return l;
        }
        l = l->next;
    }
    pl = pml;
    l = *pml;
    while (l != NULL) {
        if (jl_args_morespecific((jl_value_t*)type, (jl_value_t*)l->sig))
            break;
        if (check_amb) {
            check_ambiguous(*pml, (jl_tuple_t*)type, (jl_tuple_t*)l->sig,
                            method->linfo ? method->linfo->name :
                            anonymous_sym);
        }
        pl = &l->next;
        l = l->next;
    }
    jl_methlist_t *newrec = (jl_methlist_t*)allocobj(sizeof(jl_methlist_t));
    newrec->type = (jl_type_t*)jl_method_type;
    newrec->sig = type;
    newrec->tvars = tvars;
    newrec->va = (type->length > 0 &&
                  jl_is_seq_type(jl_tupleref(type,type->length-1))) ?
        jl_true : jl_false;
    newrec->func = method;
    newrec->invokes = NULL;
    newrec->next = l;
    JL_SIGATOMIC_BEGIN();
    *pl = newrec;
    // if this contains Union types, methods after it might actually be
    // more specific than it. we need to re-sort them.
    if (has_unions(type)) {
        jl_methlist_t *item = newrec->next, *next;
        jl_methlist_t **pitem = &newrec->next, **pnext;
        while (item != NULL) {
            pl = pml;
            l = *pml;
            next = item->next;
            pnext = &item->next;
            while (l != newrec->next) {
                if (jl_args_morespecific((jl_value_t*)item->sig,
                                         (jl_value_t*)l->sig)) {
                    // reinsert item earlier in the list
                    *pitem = next;
                    item->next = l;
                    *pl = item;
                    pnext = pitem;
                    break;
                }
                pl = &l->next;
                l = l->next;
            }
            item = next;
            pitem = pnext;
        }
    }
    JL_SIGATOMIC_END();
    return newrec;
}

static void remove_conflicting(jl_methlist_t **pl, jl_value_t *type)
{
    jl_methlist_t *l = *pl;
    while (l != NULL) {
        if (jl_type_intersection(type, (jl_value_t*)l->sig) !=
            (jl_value_t*)jl_bottom_type) {
            *pl = l->next;
        }
        else {
            pl = &l->next;
        }
        l = l->next;
    }
}

jl_methlist_t *jl_method_table_insert(jl_methtable_t *mt, jl_tuple_t *type,
                                      jl_function_t *method, jl_tuple_t *tvars)
{
    if (tvars->length == 1)
        tvars = (jl_tuple_t*)jl_t0(tvars);
    JL_SIGATOMIC_BEGIN();
    jl_methlist_t *ml = jl_method_list_insert(&mt->defs,type,method,tvars,1);
    // invalidate cached methods that overlap this definition
    remove_conflicting(&mt->cache, (jl_value_t*)type);
    if (mt->cache_arg1) {
        for(int i=0; i < jl_array_len(mt->cache_arg1); i++) {
            jl_methlist_t **pl = (jl_methlist_t**)&jl_cellref(mt->cache_arg1,i);
            if (*pl)
                remove_conflicting(pl, (jl_value_t*)type);
        }
    }
    if (mt->cache_targ) {
        for(int i=0; i < jl_array_len(mt->cache_targ); i++) {
            jl_methlist_t **pl = (jl_methlist_t**)&jl_cellref(mt->cache_targ,i);
            if (*pl)
                remove_conflicting(pl, (jl_value_t*)type);
        }
    }
    // update max_args
    jl_tuple_t *t = (jl_tuple_t*)type;
    size_t na = t->length;
    if (t->length>0 && jl_is_seq_type(jl_tupleref(t,t->length-1)))
        na--;
    if (na > jl_unbox_long(mt->max_args)) {
        mt->max_args = jl_box_long(na);
    }
    JL_SIGATOMIC_END();
    return ml;
}

jl_value_t *jl_no_method_error(jl_function_t *f, jl_value_t **args, size_t na)
{
    jl_value_t **a = alloca(sizeof(jl_value_t*)*(na+1));
    a[0] = (jl_value_t*)f;
    int i;
    for(i=0; i < na; i++)
        a[i+1] = args[i];
    return jl_apply(jl_method_missing_func, a, na+1);
}

//#define JL_TRACE
#if defined(JL_TRACE) || defined(TRACE_INFERENCE)
static char *type_summary(jl_value_t *t)
{
    if (jl_is_tuple(t)) return "Tuple";
    if (jl_is_func_type(t)) return "Function";
    if (jl_is_some_tag_type(t))
        return ((jl_tag_type_t*)t)->name->name->name;
    ios_printf(ios_stderr, "unexpected argument type: ");
    jl_show(t);
    ios_printf(ios_stderr, "\n");
    assert(0);
    return NULL;
}
#endif

static jl_tuple_t *arg_type_tuple(jl_value_t **args, size_t nargs)
{
    jl_tuple_t *tt = jl_alloc_tuple(nargs);
    JL_GC_PUSH(&tt);
    size_t i;
    for(i=0; i < tt->length; i++) {
        jl_value_t *a;
        if (jl_is_nontuple_type(args[i])) {  //***
            a = (jl_value_t*)jl_wrap_Type(args[i]);
        }
        else {
            a = (jl_value_t*)jl_full_type(args[i]);
        }
        jl_tupleset(tt, i, a);
    }
    JL_GC_POP();
    return tt;
}

jl_function_t *jl_method_lookup_by_type(jl_methtable_t *mt, jl_tuple_t *types,
                                        int cache)
{
    jl_function_t *sf = jl_method_table_assoc_exact_by_type(mt, types);
    if (sf == NULL) {
        sf = jl_mt_assoc_by_type(mt, types, cache);
    }
    return sf;
}

jl_function_t *jl_method_lookup(jl_methtable_t *mt, jl_value_t **args, size_t nargs, int cache)
{
    jl_function_t *sf = jl_method_table_assoc_exact(mt, args, nargs);
    if (sf == NULL) {
        jl_tuple_t *tt = arg_type_tuple(args, nargs);
        JL_GC_PUSH(&tt);
        sf = jl_mt_assoc_by_type(mt, tt, cache);
        JL_GC_POP();
    }
    return sf;
}

// compile-time method lookup
DLLEXPORT
jl_function_t *jl_get_specialization(jl_function_t *f, jl_tuple_t *types)
{
    assert(jl_is_gf(f));
    if (!jl_is_leaf_type((jl_value_t*)types))
        return NULL;
    jl_methtable_t *mt = jl_gf_mtable(f);
    jl_function_t *sf = jl_method_lookup_by_type(mt, types, 1);
    if (sf == NULL) {
        return NULL;
    }
    if (sf->linfo == NULL || sf->linfo->ast == NULL) {
        return NULL;
    }
    if (sf->linfo->inInference) return NULL;
    if (sf->linfo->functionObject == NULL) {
        if (sf->fptr != &jl_trampoline)
            return NULL;
        jl_compile(sf);
    }
    return sf;
}

#ifdef JL_TRACE
static int trace_en = 0;
static void enable_trace(int x) { trace_en=x; }
#endif

JL_CALLABLE(jl_apply_generic)
{
    jl_value_t *env = ((jl_function_t*)F)->env;
    jl_methtable_t *mt = (jl_methtable_t*)jl_t0(env);
#ifdef JL_GF_PROFILE
    mt->ncalls++;
#endif
#ifdef JL_TRACE
    if (trace_en) {
        ios_printf(ios_stdout, "%s(", ((jl_sym_t*)jl_t1(env))->name);
        size_t i;
        for(i=0; i < nargs; i++) {
            if (i > 0) ios_printf(ios_stdout, ", ");
            ios_printf(ios_stdout, "%s", type_summary(jl_typeof(args[i])));
        }
        ios_printf(ios_stdout, ")\n");
    }
#endif
    /*
      search order:
      look at concrete signatures
      if there is an exact match, return it
      otherwise look for a matching generic signature
      if no concrete or generic match, raise error
      if no generic match, use the concrete one even if inexact
      otherwise instantiate the generic method and use it
    */
    jl_function_t *mfunc = jl_method_table_assoc_exact(mt, args, nargs);
    if (mfunc != NULL) {
        if (mfunc->linfo != NULL && 
            (mfunc->linfo->inInference || mfunc->linfo->inCompile)) {
            // if inference is running on this function, return a copy
            // of the function to be compiled without inference and run.
            jl_lambda_info_t *li = mfunc->linfo;
            if (li->unspecialized == NULL) {
                li->unspecialized = jl_instantiate_method(mfunc, li->sparams);
            }
            mfunc = li->unspecialized;
        }
    }
    else {
        jl_tuple_t *tt = arg_type_tuple(args, nargs);
        JL_GC_PUSH(&tt);
        mfunc = jl_mt_assoc_by_type(mt, tt, 1);
        JL_GC_POP();
    }

    if (mfunc == NULL) {
        return jl_no_method_error((jl_function_t*)F, args, nargs);
    }
    assert(!mfunc->linfo || !mfunc->linfo->inInference);

    return jl_apply(mfunc, args, nargs);
}

// invoke()
// this does method dispatch with a set of types to match other than the
// types of the actual arguments. this means it sometimes does NOT call the
// most specific method for the argument types, so we need different logic.
// first we use the given types to look up a definition, then we perform
// caching and specialization within just that definition.
// every definition has its own private method table for this purpose.
//
// NOTE: assumes argument type is a subtype of the lookup type.
jl_value_t *jl_gf_invoke(jl_function_t *gf, jl_tuple_t *types,
                         jl_value_t **args, size_t nargs)
{
    assert(jl_is_gf(gf));
    jl_methtable_t *mt = jl_gf_mtable(gf);

    jl_methlist_t *m = mt->defs;
    size_t typelen = types->length;
    size_t i;
    jl_value_t *env = (jl_value_t*)jl_false;

    while (m != NULL) {
        if (m->tvars!=jl_null) {
            env = jl_type_match((jl_value_t*)types, (jl_value_t*)m->sig);
            if (env != (jl_value_t*)jl_false) break;
        }
        else if (jl_tuple_subtype(&jl_tupleref(types,0), typelen,
                                  &jl_tupleref(m->sig,0),
                                  ((jl_tuple_t*)m->sig)->length, 0, 0)) {
            break;
        }
        m = m->next;
    }

    if (m == NULL) {
        return jl_no_method_error(gf, args, nargs);
    }

    // now we have found the matching definition.
    // next look for or create a specialization of this definition.

    jl_function_t *mfunc;
    if (m->invokes == NULL)
        mfunc = NULL;
    else
        mfunc = jl_method_table_assoc_exact(m->invokes, args, nargs);
    if (mfunc != NULL) {
        if (mfunc->linfo != NULL && 
            (mfunc->linfo->inInference || mfunc->linfo->inCompile)) {
            // if inference is running on this function, return a copy
            // of the function to be compiled without inference and run.
            jl_lambda_info_t *li = mfunc->linfo;
            if (li->unspecialized == NULL) {
                li->unspecialized = jl_instantiate_method(mfunc, li->sparams);
            }
            mfunc = li->unspecialized;
        }
    }
    else {
        jl_tuple_t *tpenv=jl_null;
        jl_tuple_t *newsig=NULL;
        jl_tuple_t *tt=NULL;
        JL_GC_PUSH(&env, &newsig, &tt);

        if (m->invokes == NULL) {
            m->invokes = new_method_table();
            // this private method table has just this one definition
            jl_method_list_insert(&m->invokes->defs,m->sig,m->func,m->tvars,0);
        }

        tt = arg_type_tuple(args, nargs);

        newsig = (jl_tuple_t*)m->sig;

        if (env != (jl_value_t*)jl_false) {
            tpenv = (jl_tuple_t*)env;
            // don't bother computing this if no arguments are tuples
            for(i=0; i < tt->length; i++) {
                if (jl_is_tuple(jl_tupleref(tt,i)))
                    break;
            }
            if (i < tt->length) {
                newsig =
                    (jl_tuple_t*)jl_instantiate_type_with((jl_type_t*)m->sig,
                                                          &jl_tupleref(tpenv,0),
                                                          tpenv->length/2);
            }
        }
        mfunc = cache_method(m->invokes, tt, m->func, newsig, tpenv);
        JL_GC_POP();
    }

    JL_GC_PUSH(&mfunc);
    jl_value_t *result = jl_apply(mfunc, args, nargs);
    JL_GC_POP();
    return result;
}

static void print_methlist(char *name, jl_methlist_t *ml)
{
    ios_t *s = jl_current_output_stream();
    while (ml != NULL) {
        ios_printf(s, "%s", name);
        if (ml->tvars != jl_null) {
            if (jl_is_typevar(ml->tvars)) {
                ios_putc('{', s); jl_show((jl_value_t*)ml->tvars);
                ios_putc('}', s);
            }
            else {
                jl_show_tuple(ml->tvars, '{', '}', 0);
            }
        }
        jl_show((jl_value_t*)ml->sig);
        if (ml->func == NULL)  {
            // mark dummy cache entries
            ios_printf(s, " *");
        }
        else {
            jl_lambda_info_t *li = ml->func->linfo;
            assert(li);
            long lno = jl_unbox_long(li->line);
            if (lno > 0) {
                char *fname = ((jl_sym_t*)li->file)->name;
                ios_printf(s, " at %s:%d", fname, lno);
            }
        }
        if (ml->next != NULL)
            ios_printf(s, "\n");
        ml = ml->next;
    }
}

void jl_show_method_table(jl_function_t *gf)
{
    char *name = jl_gf_name(gf)->name;
    jl_methtable_t *mt = jl_gf_mtable(gf);
    print_methlist(name, mt->defs);
    //ios_printf(ios_stdout, "\ncache:\n");
    //print_methlist(name, mt->cache);
}

void jl_initialize_generic_function(jl_function_t *f, jl_sym_t *name)
{
    f->fptr = jl_apply_generic;
    jl_value_t *nmt = (jl_value_t*)new_method_table();
    JL_GC_PUSH(&nmt);
    f->env = (jl_value_t*)jl_tuple2(nmt, (jl_value_t*)name);
    JL_GC_POP();
}

jl_function_t *jl_new_generic_function(jl_sym_t *name)
{
    jl_function_t *f = jl_new_closure(jl_apply_generic, NULL, NULL);
    JL_GC_PUSH(&f);
    jl_initialize_generic_function(f, name);
    JL_GC_POP();
    return f;
}

void jl_add_method(jl_function_t *gf, jl_tuple_t *types, jl_function_t *meth,
                   jl_tuple_t *tvars)
{
    assert(jl_is_function(gf));
    assert(jl_is_tuple(types));
    assert(jl_is_func(meth));
    assert(jl_is_tuple(gf->env));
    assert(jl_is_mtable(jl_gf_mtable(gf)));
    if (meth->linfo != NULL)
        meth->linfo->name = jl_gf_name(gf);
    (void)jl_method_table_insert(jl_gf_mtable(gf), types, meth, tvars);
}

DLLEXPORT jl_tuple_t *jl_match_method(jl_value_t *type, jl_value_t *sig,
                                      jl_tuple_t *tvars)
{
    jl_tuple_t *env = jl_null;
    jl_value_t *ti=NULL;
    JL_GC_PUSH(&env, &ti);
    ti = jl_type_intersection_matching(type, (jl_value_t*)sig, &env, tvars);
    jl_tuple_t *result = jl_tuple2(ti, env);
    JL_GC_POP();
    return result;
}

static jl_tuple_t *match_method(jl_value_t *type, jl_function_t *func,
                                jl_tuple_t *sig, jl_tuple_t *tvars)
{
    jl_tuple_t *env = jl_null;
    jl_value_t *temp=NULL;
    jl_value_t *ti=NULL;
    JL_GC_PUSH(&env, &ti, &temp);

    ti = jl_type_intersection_matching(type, (jl_value_t*)sig, &env, tvars);
    jl_tuple_t *result = NULL;
    if (ti != (jl_value_t*)jl_bottom_type) {
        assert(func->linfo);  // no builtin methods
        jl_value_t *cenv;
        if (func->env != NULL) {
            cenv = func->env;
        }
        else {
            cenv = (jl_value_t*)jl_null;
        }
        result = jl_tuple(4, ti, env, func->linfo, cenv);
    }
    JL_GC_POP();
    return result;
}

// returns linked tuples (argtypes, static_params, lambdainfo, cloenv, next)
static jl_value_t *ml_matches(jl_methlist_t *ml, jl_value_t *type,
                              jl_sym_t *name, int lim)
{
    jl_array_t *t = (jl_array_t*)jl_an_empty_cell;
    jl_tuple_t *matc=NULL;
    JL_GC_PUSH(&t, &matc);
    int len=0;
    while (ml != NULL) {
        // a method is shadowed if type <: S <: m->sig where S is the
        // signature of another applicable method
        /*
          more generally, we can stop when the type is a subtype of the
          union of all the signatures examined so far.
        */
        matc = match_method(type, ml->func, ml->sig, ml->tvars);
        if (matc != NULL) {
            len++;
            if (lim >= 0 && len > lim) {
                JL_GC_POP();
                return jl_false;
            }
            if (len == 1) {
                t = jl_alloc_cell_1d(1);
                jl_cellref(t,0) = (jl_value_t*)matc;
            }
            else {
                jl_cell_1d_push(t, (jl_value_t*)matc);
            }
            // (type ∩ ml->sig == type) ⇒ (type ⊆ ml->sig)
            if (jl_types_equal(jl_t0(matc), type)) {
                JL_GC_POP();
                return (jl_value_t*)t;
            }
        }
        ml = ml->next;
    }
    JL_GC_POP();
    return (jl_value_t*)t;
}

void jl_add_constructors(jl_struct_type_t *t);
JL_CALLABLE(jl_f_ctor_trampoline);

// return linked tuples (t1, M1, (t2, M2, (... ()))) of types and methods.
// t is the intersection of the type argument and the method signature,
// and M is the corresponding LambdaStaticData (jl_lambda_info_t)
// lim is the max # of methods to return. if there are more return jl_false.
// -1 for no limit.
DLLEXPORT
jl_value_t *jl_matching_methods(jl_function_t *gf, jl_value_t *type, int lim)
{
    if (gf->fptr == jl_f_ctor_trampoline)
        jl_add_constructors((jl_struct_type_t*)gf);
    if (!jl_is_gf(gf)) {
        return (jl_value_t*)jl_an_empty_cell;
    }
    jl_methtable_t *mt = jl_gf_mtable(gf);
    jl_sym_t *gfname = jl_gf_name(gf);
    return ml_matches(mt->defs, type, gfname, lim);
}

DLLEXPORT
int jl_is_builtin(jl_value_t *v)
{
    return ((jl_is_func(v) && (((jl_function_t*)v)->linfo==NULL) &&
             !jl_is_gf(v)) ||
            jl_typeis(v,jl_intrinsic_type));
}

DLLEXPORT
int jl_is_genericfunc(jl_value_t *v)
{
    return (jl_is_func(v) && jl_is_gf(v));
}

DLLEXPORT
jl_sym_t *jl_genericfunc_name(jl_value_t *v)
{
    return jl_gf_name(v);
}
