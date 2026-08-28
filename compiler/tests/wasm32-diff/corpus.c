/* Differential-testing corpus for the wasm32 backend's control-flow
 * structuring -- see docs/wasm-backend-size-2026-08-28.md's own
 * "prerequisite" section for why this exists: every wasm-affecting
 * codegen change so far has been verified by rv64.wasm byte-identity,
 * which is exactly the wrong tool for a change that is SUPPOSED to
 * change the emitted bytes (a stackifier fix, or eventually a
 * stack-native rewrite). This corpus is compiled once with a trusted
 * reference compiler (host gcc) and once with wasm32 tcc, both runs
 * compared function-by-function -- see run.sh.
 *
 * Every function here is non-static (tccwasm.c auto-exports every
 * non-static top-level function -- see tcc_output_wasm()'s own
 * "exports: memory + non-static functions" comment), takes no
 * arguments (so both the native driver and the JS harness can call
 * them generically, in the same textual order), and returns a plain
 * `int` (so JS's WebAssembly call returns a plain Number, not a
 * BigInt, keeping the diff a trivial decimal-string comparison).
 *
 * Deliberately weighted toward the two representability gaps found in
 * docs/wasm-codegen-rethink-2026-08-27.md: a for-loop's rotated layout
 * (condition-test and increment as two separately-jumped-to positions)
 * and switch-statement case dispatch (bodies emitted before the
 * compare-and-jump code that reaches them, colliding with loop
 * detection) -- plus everything else that pattern this project's real
 * C actually uses (see emulator/rv64.c): goto-as-shared-epilogue,
 * switch nested in a loop, loop nested in a switch case, and a
 * switch-heavy "instruction decoder" shaped exactly like execute(). */

/* ---------------------------------------------------------------- */
/* Plain loops -- the for-rotation family */

int test_for_simple(void)
{
    int i, sum = 0;
    for (i = 0; i < 10; i++)
        sum += i;
    return sum; /* 45 */
}

int test_for_complex_increment(void)
{
    /* the exact shape that trips the rotation issue: a non-trivial
     * increment expression forces tccgen.c to emit the increment
     * before the body, with a forward jump skipping it on entry --
     * see TOK_FOR's own comment in tccgen.c. */
    int i, j, sum = 0;
    for (i = 0, j = 100; i < 10; i++, j -= 3)
        sum += i + j;
    return sum; /* sum(0..9)=45, sum(100,97,...,73)=865 -> 910 */
}

int test_for_empty_increment(void)
{
    int i, sum = 0;
    for (i = 0; i < 10;) {
        sum += i;
        i++;
    }
    return sum; /* 45 */
}

int test_for_empty_body(void)
{
    int i;
    for (i = 0; i < 1000; i++)
        ;
    return i; /* 1000 */
}

int test_while_simple(void)
{
    int i = 0, sum = 0;
    while (i < 10) {
        sum += i;
        i++;
    }
    return sum; /* 45 */
}

int test_do_while(void)
{
    int i = 0, sum = 0;
    do {
        sum += i;
        i++;
    } while (i < 10);
    return sum; /* 45 */
}

int test_do_while_once(void)
{
    int i = 0, sum = 0;
    do {
        sum += 100;
    } while (i++ < 0 && 0); /* body always runs exactly once */
    return sum; /* 100 */
}

/* ---------------------------------------------------------------- */
/* break / continue, including nested and via goto (C has no labeled
 * break/continue, so "break out of two loops" is a real goto idiom) */

int test_break(void)
{
    int i, sum = 0;
    for (i = 0; i < 100; i++) {
        if (i == 10)
            break;
        sum += i;
    }
    return sum; /* 45 */
}

int test_continue(void)
{
    int i, sum = 0;
    for (i = 0; i < 10; i++) {
        if (i % 2 == 0)
            continue;
        sum += i;
    }
    return sum; /* 1+3+5+7+9 = 25 */
}

int test_nested_loops(void)
{
    int i, j, sum = 0;
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            sum += i * j;
    return sum; /* (0+1+2+3+4)^2 = 100 */
}

int test_nested_break(void)
{
    /* inner break must only exit the inner loop */
    int i, j, sum = 0;
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 5; j++) {
            if (j == 2)
                break;
            sum += 1;
        }
    }
    return sum; /* 5 * 2 = 10 */
}

int test_goto_break_nested(void)
{
    /* the real idiom for "break out of two loops at once" */
    int i, j, sum = 0;
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            if (i == 3 && j == 3)
                goto done;
            sum++;
        }
    }
done:
    return sum; /* i=0..2 contribute 10 each (30), i=3 contributes j=0,1,2 (3) = 33 */
}

int test_continue_in_while(void)
{
    int i = 0, sum = 0;
    while (i < 10) {
        i++;
        if (i % 3 == 0)
            continue;
        sum += i;
    }
    return sum; /* 1+2+4+5+7+8+10 = 37 */
}

int test_continue_in_do_while(void)
{
    int i = 0, sum = 0;
    do {
        i++;
        if (i % 3 == 0)
            continue;
        sum += i;
    } while (i < 10);
    return sum; /* 37, same as above */
}

/* ---------------------------------------------------------------- */
/* goto -- forward, backward, and shared-epilogue (the execute() shape) */

int test_goto_forward(void)
{
    int x = 1;
    goto skip;
    x = 2; /* skipped */
skip:
    x += 10;
    return x; /* 11 */
}

int test_goto_backward_loop(void)
{
    /* a hand-rolled loop via backward goto, distinct from any while/for
     * construct -- exercises gjmp_addr with no gjmp_hint_loop_range at
     * all (this is neither a loop construct nor switch dispatch). */
    int i = 0, sum = 0;
top:
    sum += i;
    i++;
    if (i < 10)
        goto top;
    return sum; /* 45 */
}

int test_goto_shared_epilogue(void)
{
    /* mirrors emulator/rv64.c's execute(): a switch where several
     * cases jump to shared cleanup code placed after the switch. */
    int op = 3, a = 10, b = 20, result;
    switch (op) {
    case 1:
        result = a + b;
        goto write;
    case 2:
        result = a - b;
        goto write;
    case 3:
        result = a * b;
        goto write;
    case 4:
        result = a / b;
        goto write;
    default:
        result = -1;
        goto write;
    }
write:
    return result; /* 200 */
}

/* ---------------------------------------------------------------- */
/* switch -- dense, sparse, fallthrough, default, nested */

int test_switch_dense(void)
{
    int i, sum = 0;
    for (i = 0; i < 6; i++) {
        switch (i) {
        case 0: sum += 1; break;
        case 1: sum += 2; break;
        case 2: sum += 4; break;
        case 3: sum += 8; break;
        case 4: sum += 16; break;
        default: sum += 100; break;
        }
    }
    return sum; /* 1+2+4+8+16+100 = 131 */
}

int test_switch_sparse(void)
{
    int i, sum = 0;
    for (i = 0; i < 5; i++) {
        int v;
        switch (i * 137) {
        case 0: v = 1; break;
        case 137: v = 2; break;
        case 274: v = 4; break;
        case 5000: v = 8; break; /* never matches -- forces binary search */
        default: v = 100; break;
        }
        sum += v;
    }
    return sum; /* i=0->1, i=1->2, i=2->4, i=3(411)->100, i=4(548)->100 = 207 */
}

int test_switch_fallthrough(void)
{
    int i, sum = 0;
    for (i = 0; i < 4; i++) {
        switch (i) {
        case 0:
        case 1:
            sum += 1; /* falls through from 0 */
            break;
        case 2:
            sum += 10; /* falls into case 3 */
        case 3:
            sum += 100;
            break;
        }
    }
    return sum; /* i=0:1, i=1:1, i=2:10+100=110, i=3:100 -> 212 */
}

int test_switch_no_default(void)
{
    int i, sum = 0;
    for (i = 0; i < 5; i++) {
        switch (i) {
        case 1: sum += 10; break;
        case 3: sum += 30; break;
        }
    }
    return sum; /* i=1:10, i=3:30 -> 40 */
}

int test_switch_loop_in_case(void)
{
    int op = 2, sum = 0;
    switch (op) {
    case 1:
        sum = 1;
        break;
    case 2: {
        int i;
        for (i = 0; i < 10; i++)
            sum += i;
        break;
    }
    default:
        sum = -1;
    }
    return sum; /* 45 */
}

int test_loop_switch_in_body(void)
{
    int i, sum = 0;
    for (i = 0; i < 6; i++) {
        switch (i % 3) {
        case 0: sum += 1; break;
        case 1: sum += 10; break;
        case 2: sum += 100; break;
        }
    }
    return sum; /* i=0:1,1:10,2:100,3:1,4:10,5:100 -> 222 */
}

int test_switch_nested(void)
{
    int outer, inner, sum = 0;
    for (outer = 0; outer < 3; outer++) {
        switch (outer) {
        case 0:
            for (inner = 0; inner < 3; inner++) {
                switch (inner) {
                case 0: sum += 1; break;
                case 1: sum += 2; break;
                default: sum += 4; break;
                }
            }
            break;
        case 1:
            sum += 100;
            break;
        default:
            sum += 1000;
            break;
        }
    }
    return sum; /* outer=0: 1+2+4=7, outer=1: 100, outer=2: 1000 -> 1107 */
}

int test_switch_break_vs_loop_break(void)
{
    /* break inside a switch inside a loop must exit the SWITCH, not
     * the loop -- a real, easy-to-get-wrong interaction. */
    int i, sum = 0;
    for (i = 0; i < 5; i++) {
        switch (i) {
        case 2:
            sum += 1000; /* only i==2 should ever add this */
            break;
        }
        sum += 1; /* must run every iteration, including i==2 */
    }
    return sum; /* 5*1 + 1000 = 1005 */
}

/* ---------------------------------------------------------------- */
/* short-circuit evaluation order and side effects */

static int g_trace;

static int trace_and_return(int tag, int v)
{
    g_trace = g_trace * 10 + tag;
    return v;
}

int test_short_circuit_and(void)
{
    g_trace = 0;
    if (trace_and_return(1, 0) && trace_and_return(2, 1)) {
        /* unreachable */
    }
    return g_trace; /* only tag 1 evaluated: 1 */
}

int test_short_circuit_or(void)
{
    g_trace = 0;
    if (trace_and_return(1, 1) || trace_and_return(2, 1)) {
        /* reached, but tag 2 must NOT have been evaluated */
    }
    return g_trace; /* only tag 1 evaluated: 1 */
}

int test_ternary_side_effect(void)
{
    int a = 5;
    int r = (a > 0) ? (a * 2) : (a * -2);
    return r; /* 10 */
}

/* ---------------------------------------------------------------- */
/* A realistic instruction-decoder shape, matching execute()'s own:
 * a large switch, every case ending in a shared epilogue via goto,
 * dispatching on an opcode computed in a loop -- the combination that
 * silently took the slow path for real, in this project's own boot
 * test (see docs/wasm-codegen-rethink-2026-08-27.md). */

static int decode_one(int opcode, int a, int b)
{
    int result;
    switch (opcode) {
    case 0: result = a + b; goto epilogue;
    case 1: result = a - b; goto epilogue;
    case 2: result = a & b; goto epilogue;
    case 3: result = a | b; goto epilogue;
    case 4: result = a ^ b; goto epilogue;
    case 5:
        if (b == 0) { result = 0; goto epilogue; }
        result = a / b;
        goto epilogue;
    case 6: result = (a < b) ? 1 : 0; goto epilogue;
    default: result = -1; goto epilogue;
    }
epilogue:
    return result;
}

int test_instruction_decoder(void)
{
    int i, sum = 0;
    for (i = 0; i < 8; i++)
        sum += decode_one(i, 12, 5);
    /* op0=17 op1=7 op2=4 op3=13 op4=9 op5=2 op6=0 op7(default)=-1 */
    return sum; /* 17+7+4+13+9+2+0-1 = 51 */
}

/* ---------------------------------------------------------------- */
/* Two DIFFERENT plain locals combined directly (not an accumulator
 * pattern like `sum += i`) -- specifically targeting the case where
 * neither operand of a binary op or comparison ever touches a wasm
 * "register" at all. Non-commutative operators are the load-bearing
 * ones here: they only give the right answer if operand push order
 * (left, then right) survives being emitted inline instead of via two
 * separately-materialized registers. */

int test_two_locals_add(void)
{
    int a = 17, b = 25;
    return a + b; /* 42 */
}

int test_two_locals_sub(void)
{
    int a = 100, b = 37;
    return a - b; /* 63 -- wrong operand order gives -63 */
}

int test_two_locals_div(void)
{
    int a = 97, b = 4;
    return a / b; /* 24 -- wrong operand order gives 0 */
}

int test_two_locals_mod(void)
{
    int a = 97, b = 4;
    return a % b; /* 1 -- wrong operand order gives 4 */
}

int test_two_locals_shift(void)
{
    int a = 1, b = 5;
    return a << b; /* 32 -- wrong operand order gives 0 (5 << 1 truncated? no: 1<<5=32, 5<<1=10) */
}

int test_two_locals_cmp_lt(void)
{
    int a = 3, b = 9;
    return a < b; /* 1 -- wrong operand order gives 0 */
}

int test_two_locals_cmp_gt(void)
{
    int a = 3, b = 9;
    return a > b; /* 0 -- wrong operand order gives 1 */
}

int test_two_locals_chain(void)
{
    /* (a+b) + (c+d): the OUTER combine's left operand is itself the
     * freshly-computed result of an inner both-locals combine (now
     * living in a real register, get_reg()'d), its right operand is
     * ANOTHER both-locals combine -- checks that a both-locals result
     * correctly feeds into being the ordinary left operand of a
     * further combine. */
    int a = 1, b = 2, c = 3, d = 4;
    return (a + b) + (c + d); /* 10 */
}

int test_two_locals_in_loop(void)
{
    /* Same two named locals combined every iteration, inside a loop --
     * checks that get_reg() correctly finds a free register on every
     * pass rather than colliding with the loop's own induction
     * variable or exit test. */
    int i, x = 3, y = 5, sum = 0;
    for (i = 0; i < 10; i++)
        sum += x - y; /* -2 each time */
    return sum; /* -20 */
}
