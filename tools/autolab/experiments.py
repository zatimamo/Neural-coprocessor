"""MGPU-AutoLab - the predefined experiment tree, as DATA.

WHAT THIS FILE IS
    The decisions AutoLab makes are here, declared, not written as prose in a
    controller and not chosen at run time. The controller walks this graph and
    does exactly three things with it: run the node it points at, evaluate the
    node's rules against the result, and follow the next pointer the first
    matching rule names.

WHAT THIS FILE IS NOT
    It is not a place to add a hypothesis. AutoLab may not invent one, and the
    controller has no branch that can express one. A result that matches no rule
    is UNEXPECTED_MIXED_STATE and AutoLab stops - it does not try the next
    thing that looks plausible.

THE RULES ARE DATA
    Each rule is a list of conditions over a flat view of the result. The
    supported predicates are the whole vocabulary:

        observed      the field has a value (not None)
        is_null       the field is None
        eq / ne       equal / not equal to `value`, and NOT satisfied when the
                      field is None - an unobserved field never satisfies a rule
        is_true       exactly True
        is_false      exactly False
        is_not_true   False OR None: "did not reproduce", which is the wording
                      the PROCESSCONTEXT reference gate uses
        is_zero / is_not_zero

    There is no expression evaluation, no `eval`, and no callable in the graph.
"""

# ---------------------------------------------------------------------------
# Workflows and artifacts. The artifact AutoLab downloads is ALWAYS tied to the
# exact run it triggered; "latest artifact" is never accepted.
# ---------------------------------------------------------------------------

WORKFLOWS = {
    "PROCESSCONTEXT": {
        "workflow": "processcontext-ab.yml",
        "artifact": "processcontext-ab",
        "kind": "standalone",
        "description": "four-arm multi-device context diagnostic (no game)",
    },
    "APPID_NS": {
        "workflow": "appid-ns.yml",
        "artifact": "mgpu_appid_ns",
        "kind": "game",
        "description": "NGX ApplicationId 0x1000000 for the Reserved18 sessions",
        "marker": "[APPID]",
    },
    "RESHADENATIVE": {
        "workflow": "reshadenative-ab.yml",
        "artifact": "mgpu_reshade_native",
        "kind": "game",
        "description": "P1.0c NGX lane on the unwrapped original D3D12 device",
        "marker": "[RESHADENATIVE]",
    },
    "CUDADIAG": {
        "workflow": "cudadiag-ab.yml",
        "artifact": "mgpu_cudadiag",
        "kind": "game",
        "description": "pass-through instrumentation of the two CUDA-interop calls",
        "marker": "[CUDADIAG]",
    },
}

#: The add-on inside every game artifact's deploy set. Fixed by the deploy
#: convention: the artifact is uploaded as <name>/nvngx.dll_mgpu_bridge.addon64.
ADDON_IN_ARTIFACT = "nvngx.dll_mgpu_bridge.addon64"

# ---------------------------------------------------------------------------
# Game-experiment gates. Both are declarations, evaluated by autolab.evaluate.
# ---------------------------------------------------------------------------

#: A game run is a valid basis only if BOTH Reserved18 sessions initialised.
#: FAIL_OutOfDate voids the NGX portion of the run - this is the same run-time
#: gate every phase of this investigation has used.
GAME_VALIDITY_GATE = [
    {"field": "p1_core_init", "op": "eq", "value": 0x00000001},
    {"field": "p4_core_init", "op": "eq", "value": 0x00000001},
]

#: Observations a game run must produce before it is treated as complete and the
#: game is closed. Until every one of these is observed, AutoLab keeps tailing.
#: A field that never appears is NOT a result: the run is INVALID/TIMEOUT.
GAME_REQUIRED_FIELDS = [
    "p1_core_init",
    "p4_core_init",
    "descriptor_real_status",
    "cumodule_real_status",
    "p1_create_feature",
]

# ---------------------------------------------------------------------------
# PROCESSCONTEXT.
# ---------------------------------------------------------------------------

#: THE MANDATORY REFERENCE GATE, for the SINGLE arm.
#:
#: SINGLE is the control. It runs the NeuralScreen-equivalent lane in a process
#: with no second-adapter state at all, and it must reproduce the working result
#: exactly. If it does not, no other arm of this diagnostic is readable: the
#: reference itself did not reproduce, so nothing can be attributed to the
#: variable the other three arms introduce.
#:
#: Every condition must hold AND be observed. `is_not_zero` on the feature
#: handle fails on an unobserved handle, because a handle that was not read is
#: not a handle.
#:
#: These are the eight conditions the corrected PROCESSCONTEXT control lane
#: implements, evaluated here INDEPENDENTLY from the arm's machine-readable
#: result line. The diagnostic reaches its own verdict from the same eight
#: fields, so agreement between the two is a check on both - one of them
#: computing it wrongly shows up as a disagreement rather than as a result.
PROCESSCONTEXT_REFERENCE_GATE = [
    # core NVSDK_NGX_D3D12_Init            = Success
    {"field": "core_init", "op": "eq", "value": 0x00000001},
    # NVSDK_NGX_D3D12_AllocateParameters   = Success
    {"field": "alloc", "op": "eq", "value": 0x00000001},
    # snippet NVSDK_NGX_D3D12_Init_Ext     = Success
    {"field": "snip_init", "op": "eq", "value": 0x00000001},
    # GetCudaIndependentDescriptorObject   real status = 0
    {"field": "descriptor_status", "op": "eq", "value": 0},
    # CreateCuModule                       real status = 0
    {"field": "cumodule_status", "op": "eq", "value": 0},
    # first CuModule blob size             = 3944768
    {"field": "blob_size", "op": "eq", "value": 3944768},
    # CreateFeature Reserved18             = Success
    {"field": "feature", "op": "eq", "value": 0x00000001},
    # the feature handle                   != 0
    {"field": "feature_handle", "op": "is_not_zero"},
]

#: The four arms, in the order they are run. Each is a fresh process.
PROCESSCONTEXT_ARMS = ["single", "dual-held", "dual-active", "dual-released"]

#: The arm whose job is to reproduce the reference. Named here rather than in the
#: controller so the graph and the gate agree about which arm that is.
PROCESSCONTEXT_REFERENCE_ARM = "single"

# ---------------------------------------------------------------------------
# THE DECISION GRAPH
# ---------------------------------------------------------------------------

GRAPH = {
    "start": "PROCESSCONTEXT",
    "nodes": {

        # ------------------------------------------------------------------
        "PROCESSCONTEXT": {
            "experiment": "PROCESSCONTEXT",
            "implemented": True,
            "workflow": "PROCESSCONTEXT",
            "summary_rows": ["PROCESS/SINGLE", "PROCESS/DUAL-HELD",
                             "PROCESS/DUAL-ACTIVE", "PROCESS/DUAL-RELEASED"],
            "decisions": [
                # The reference did not reproduce. Stop. Interpret nothing.
                {"id": "REFERENCE_INVALID",
                 "verdict": "PROCESSCONTEXT_REFERENCE_INVALID",
                 "next": None, "stop": True,
                 "because": "the SINGLE reference arm did not reproduce the working result, "
                            "so no other arm of this diagnostic is readable",
                 "when": [{"field": "arms.single.reference_ok", "op": "is_not_true"}]},

                # SINGLE ok, the held device alone breaks it.
                {"id": "SECOND_LIVE_D3D12_DEVICE_SUFFICIENT",
                 "verdict": "SECOND_LIVE_D3D12_DEVICE_SUFFICIENT",
                 "next": None, "stop": True,
                 "because": "a second live D3D12 device is present and the lane fails; "
                            "no second queue, heap or NGX use was required",
                 "when": [{"field": "arms.single.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-held.reference_ok", "op": "is_false"}]},

                # The device alone is not enough; live command/descriptor state is.
                {"id": "ACTIVE_SECOND_DEVICE_STATE_SUFFICIENT",
                 "verdict": "ACTIVE_SECOND_DEVICE_STATE_SUFFICIENT",
                 "next": None, "stop": True,
                 "because": "the second device alone is tolerated and an active second "
                            "DIRECT queue with a live descriptor heap is not",
                 "when": [{"field": "arms.single.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-held.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-active.reference_ok", "op": "is_false"}]},

                # State outlives the device.
                {"id": "PROCESS_GLOBAL_STATE_PERSISTS_AFTER_DEVICE_RELEASE",
                 "verdict": "PROCESS_GLOBAL_STATE_PERSISTS_AFTER_DEVICE_RELEASE",
                 "next": None, "stop": True,
                 "because": "the failure survives releasing the second device, so the "
                            "residual state is process-global and outlives it",
                 "when": [{"field": "arms.single.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-held.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-active.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-released.reference_ok", "op": "is_false"}]},

                # Multi-device state is exonerated. The next class of experiment
                # does not exist yet, and AutoLab stops here rather than inventing
                # one.
                {"id": "MULTI_DEVICE_EXONERATED",
                 "verdict": "MULTI_DEVICE_EXONERATED",
                 "next": "STREAMLINE_CONTEXT_REQUIRED", "stop": False,
                 "because": "all four arms reproduced the working result, so neither the "
                            "presence of a second device, nor active second-device state, "
                            "nor residual state after its release explains the failure",
                 "when": [{"field": "arms.single.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-held.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-active.reference_ok", "op": "is_true"},
                          {"field": "arms.dual-released.reference_ok", "op": "is_true"}]},
            ],
            # Reached when the arms produced a combination no rule describes -
            # including an arm that did not run, or a parse conflict. STOP.
            "fallback": {
                "id": "UNEXPECTED_MIXED_STATE",
                "verdict": "UNEXPECTED_MIXED_STATE",
                "next": None, "stop": True,
                "because": "the arms did not produce one of the five predefined "
                           "combinations, so no predefined verdict applies",
            },
        },

        # ------------------------------------------------------------------
        # A node with no implementation, on purpose. It exists so the graph can
        # name where the investigation goes next without AutoLab inventing or
        # executing anything.
        "STREAMLINE_CONTEXT_REQUIRED": {
            "experiment": "STREAMLINE_CONTEXT_REQUIRED",
            "implemented": False,
            "placeholder": True,
            "verdict": "STREAMLINE_CONTEXT_REQUIRED",
            "next": None,
            "because": "process-global multi-device D3D12 state is exonerated; the next "
                       "unimplemented experiment class is Streamline / Cyberpunk NVIDIA "
                       "process initialization",
            "decisions": [],
            "fallback": {"id": "PLACEHOLDER", "verdict": "STREAMLINE_CONTEXT_REQUIRED",
                         "next": None, "stop": True},
        },
    },
}

#: A guard against a malformed graph. The controller also counts steps.
MAX_STEPS = 32

# ---------------------------------------------------------------------------
# IMPORTED HISTORICAL RESULTS
# ---------------------------------------------------------------------------
# Results that were measured before AutoLab existed and are entered as data.
# They are reported in the summary and are NOT re-run and NOT re-interpreted.
#
# APPID_NS: measured VALID NEGATIVE. Both Reserved18 sessions initialised
# (Success) and the failure was unchanged - the ApplicationId is exonerated.
#
# The measured descriptor/CuModule/CreateFeature values are recorded under
# `measured` exactly as supplied. They are NOT split across p1 and p4, because
# the source did not split them, and attributing them to one phase would be
# inventing detail the measurement does not contain.

IMPORTED_RESULTS = [
    {
        "experiment": "APPID_NS",
        "valid": True,
        "verdict": "EXONERATED",
        "reason": "measured VALID NEGATIVE: P1 Init Success, P4 Init Success, "
                  "descriptor -1, CuModule -1, CreateFeature 0xBAD00002",
        "p1": {"init": "Success", "descriptor": None, "cumodule": None,
               "create_feature": None},
        "p4": {"init": "Success", "descriptor": None, "cumodule": None,
               "create_feature": None},
        "next": None,
        "imported": True,
        "measured": {
            "p1_init": "Success",
            "p4_init": "Success",
            "descriptor": -1,
            "cumodule": -1,
            "create_feature": "0xBAD00002",
        },
        "source": "operator-supplied historical measurement, imported into the graph "
                  "so that AutoLab begins at the corrected PROCESSCONTEXT experiment; "
                  "not re-run by AutoLab",
    },
]

#: The summary prints imported results first, in this order, then the rows the
#: run itself produced.
IMPORTED_ROW_ORDER = ["APPID_NS"]


# ---------------------------------------------------------------------------
# Game result construction
# ---------------------------------------------------------------------------

def required_complete(experiment_name, parser):
    """True when every observation this experiment must produce is present.

    Used by the live tail as the stop condition. A field that never appears is
    not a result: the run then ends in INVALID/TIMEOUT, which the caller
    reports as such.
    """
    obs = parser.observation_summary()
    for field in GAME_REQUIRED_FIELDS:
        if obs.get(field) is None:
            return False
    marker = (WORKFLOWS.get(experiment_name) or {}).get("marker")
    if marker:
        flags = {"[APPID]": obs.get("appid_lines"),
                 "[RESHADENATIVE]": obs.get("reshadenative_lines"),
                 "[CUDADIAG]": [obs.get("cudadiag_variant")] if obs.get("cudadiag_variant") else []}
        if not flags.get(marker):
            return False
    return True


def build_game_result(experiment_name, parser, artifact_meta, run_info):
    """Assemble the common result object from a parsed ReShade.log.

    p1 and p4 are filled from CUDADIAG's own scope labels, so a phase field is
    only ever set from a call the add-on attributed to that phase. A field with
    no such call is None - never carried over from the other phase, never
    defaulted to zero.
    """
    phases = parser.phase_observations()
    obs = parser.observation_summary()
    obs["phases"] = phases
    obs["artifact"] = artifact_meta
    obs["run"] = run_info
    # Flatten the phase-scoped fields so the validity gate and any rule can name
    # them directly.
    flat = {"p1_core_init": parser.p1_core_init,
            "p4_core_init": parser.p4_core_init,
            "p1_create_feature": parser.p1_create_feature,
            "p4_create_feature": parser.p4_create_feature}
    for phase in ("p1", "p4"):
        for kind in ("descriptor", "cumodule"):
            flat["%s_%s_status" % (phase, kind)] = phases[phase][kind]
            detail = phases["detail"]["%s_%s" % (phase, kind)]
            flat["%s_%s_handle" % (phase, kind)] = detail["handle"]
            flat["%s_%s_observed" % (phase, kind)] = detail["observed"]
        flat["%s_init" % phase] = phases[phase]["init"]
    flat["descriptor_real_status"] = obs.get("descriptor_real_status")
    flat["cumodule_real_status"] = obs.get("cumodule_real_status")
    flat["descriptor_real_handle"] = obs.get("descriptor_real_handle")
    flat["cumodule_real_handle"] = obs.get("cumodule_real_handle")
    flat["cumodule_blob_size"] = obs.get("cumodule_blob_size")

    ok, failing = gate_passes(GAME_VALIDITY_GATE, flat)
    if ok:
        valid, reason = True, ("both Reserved18 sessions initialised: P1 Init and P4 Init "
                               "are Success")
    else:
        valid = False
        reason = "the run-time validity gate failed: " + "; ".join(
            "%s (need %s %r, saw %r)" % (c["field"], c["op"], c.get("value"),
                                         get_field(flat, c["field"]))
            for c in failing)

    return {
        "experiment": experiment_name,
        "valid": valid,
        "reason": reason,
        "p1": phases["p1"],
        "p4": phases["p4"],
        "verdict": "",
        "next": "",
        "observations": obs,
        "flat": flat,
    }


# ---------------------------------------------------------------------------
# Predicate evaluation
# ---------------------------------------------------------------------------

class GraphError(Exception):
    pass


def get_field(data, path):
    """Resolve a dotted path. A missing part is None, never an exception."""
    cur = data
    for part in path.split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            return None
    return cur


def evaluate(condition, data):
    """Evaluate one declared condition. Unknown ops are an error, not a False."""
    field = condition.get("field")
    op = condition.get("op")
    value = get_field(data, field)

    if op == "observed":
        return value is not None
    if op == "is_null":
        return value is None
    if op == "eq":
        # An unobserved field never satisfies an equality: a missing observation
        # must not be able to look like a matched one.
        return value is not None and value == condition.get("value")
    if op == "ne":
        return value is not None and value != condition.get("value")
    if op == "is_true":
        return value is True
    if op == "is_false":
        return value is False
    if op == "is_not_true":
        return value is not True
    if op == "is_zero":
        return value is not None and value == 0
    if op == "is_not_zero":
        return value is not None and value != 0
    raise GraphError("unknown predicate op %r (field=%r)" % (op, field))


def gate_passes(gate, data):
    """True only if every condition holds. Returns (ok, failing_conditions)."""
    failing = [c for c in gate if not evaluate(c, data)]
    return (len(failing) == 0), failing


class Decision(object):
    __slots__ = ("id", "verdict", "next", "stop", "because")

    def __init__(self, id_, verdict, next_, stop, because=""):
        self.id = id_
        self.verdict = verdict
        self.next = next_
        self.stop = stop
        self.because = because

    def as_dict(self):
        return {"id": self.id, "verdict": self.verdict, "next": self.next,
                "stop": self.stop, "because": self.because}


def decide(node, data):
    """The first declared rule whose conditions all hold; else the fallback."""
    for rule in node.get("decisions", []):
        if all(evaluate(c, data) for c in rule["when"]):
            stop = rule.get("stop")
            if stop is None:
                stop = rule.get("next") is None
            return Decision(rule["id"], rule["verdict"], rule.get("next"),
                            bool(stop), rule.get("because", ""))
    fb = node["fallback"]
    stop = fb.get("stop")
    if stop is None:
        stop = fb.get("next") is None
    return Decision(fb["id"], fb["verdict"], fb.get("next"), bool(stop),
                    fb.get("because", ""))


def node(name):
    if name not in GRAPH["nodes"]:
        raise GraphError("no such experiment node: %r" % (name,))
    return GRAPH["nodes"][name]


def validate_graph():
    """Structural check over the graph data. Run at startup, before anything."""
    problems = []
    for name, n in GRAPH["nodes"].items():
        if n.get("experiment") != name:
            problems.append("%s: experiment field is %r" % (name, n.get("experiment")))
        if n.get("implemented") and not n.get("decisions"):
            problems.append("%s: implemented but has no decision rules" % name)
        if "fallback" not in n:
            problems.append("%s: no fallback rule" % name)
        for rule in n.get("decisions", []):
            for key in ("id", "verdict", "when"):
                if key not in rule:
                    problems.append("%s: rule %r is missing %r" % (name, rule.get("id"), key))
            nxt = rule.get("next")
            if nxt is not None and nxt not in GRAPH["nodes"]:
                problems.append("%s: rule %s points at unknown node %r"
                                % (name, rule.get("id"), nxt))
            # A rule must not be able to fall through to another rule silently:
            # every condition is evaluated, and an unknown op is fatal.
            for cond in rule.get("when", []):
                if cond.get("op") not in ("observed", "is_null", "eq", "ne", "is_true",
                                          "is_false", "is_not_true", "is_zero",
                                          "is_not_zero"):
                    problems.append("%s: rule %s has unknown op %r"
                                    % (name, rule.get("id"), cond.get("op")))
        fb = n["fallback"]
        if fb.get("next") is not None and fb["next"] not in GRAPH["nodes"]:
            problems.append("%s: fallback points at unknown node %r" % (name, fb["next"]))
    if GRAPH["start"] not in GRAPH["nodes"]:
        problems.append("start node %r is not in the graph" % GRAPH["start"])
    return problems
