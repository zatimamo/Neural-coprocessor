"""MGPU-AutoLab - parsers for ReShade.log and the PROCESSCONTEXT arm logs.

WHY THIS FILE IS THE ONE THAT MATTERS
    Everything downstream of a run - the validity gate, the decision graph, the
    summary - reads fields this module produced. A parser that invents a value,
    or that quietly defaults a missing one, turns a non-result into a result and
    the whole orchestration into fiction. So the rules here are strict:

      * A field that was not observed is None. Never 0, never a default, never
        carried over from another call.
      * A NULL-argument capability probe is not a call. A probe that returns -14
        (NVAPI_INVALID_POINTER) is the expected answer to a validation probe and
        is recorded separately; it can never become a "real status".
      * Only the FIRST REAL call of each kind carries the status this
        investigation is about, and a probe does not consume that slot.
      * Where two independent sources describe the same fact - the per-arm
        machine-readable result line and the detailed [cuda] lines - they are
        cross-checked, and a disagreement is reported as a parse conflict rather
        than resolved by preferring one.

The exact source formats this parses were read out of the add-on and the
diagnostic, not guessed:

    [MGPU][P1.0c] Init: result=0x00000001 (NVSDK_NGX_Result_Success) device=...
    [MGPU][P1.0c] Init_Ext (FALLBACK - no log callback installed): result=0x...
    [MGPU][P1.0c] snippet Init_Ext: result=0x00000001 (...) device=... params=...
    [MGPU][P4.1] Init: result=0x00000001 (NVSDK_NGX_Result_Success)
    [MGPU][P1.0c] CreateFeature(Reserved18) via snippet: result=0x... handle=0x...
    [MGPU][P4.1] CreateFeature(Reserved18) handle 1/2 640x360 fmt=10: result=0x... handle=0x...
    [CUDADIAG] GetCudaIndependentDescriptorObject seq=1 tid=... scope=... mismatch_seen=no pParams=0x...
    [CUDADIAG]   structSizeIn=48 structSizeOut=0
    [CUDADIAG]   type=0 (SURFACE)
    [CUDADIAG]   desc.ptr=0x1A4BAD70000
    [CUDADIAG] GetCudaIndependentDescriptorObject status=-1 (0xFFFFFFFF)
    [CUDADIAG]   returned handle=0x1A4BAD7000
    [CUDADIAG] CreateCuModule seq=1 tid=... scope=... mismatch_seen=no pBlob=0x... size=3944768 phModule=0x...
    [CUDADIAG] CreateCuModule status=-1 (0xFFFFFFFF)
    [CUDADIAG]   returned module handle=0x000000000000A000
    [CUDADIAG] armed GetCudaIndependentDescriptorObject real=0x... id=0x0DDAC234
    [CUDADIAG] variant=CUDA_DIAG_ON reachable=yes ...

    [cuda] #1 GetCudaIndependentDescriptorObject 0x0DDAC234 REAL pDevice=0x... type=0 desc.ptr=0x... structSizeIn=48 structSizeOut_before=0 match=yes
    [cuda] #1 REAL status=0 (0x00000000) structSizeOut_after=48 returned_handle=0x1A4BAD7000
    [cuda] #2 CreateCuModule 0xAD1A677D REAL pDevice=0x... pBlob=0x... (pointer only; never dereferenced) size=3944768 phModule=0x... match=yes
    [cuda] #2 REAL status=0 (0x00000000) module_handle=0xA000
    [cuda] first CreateCuModule blob size = 3944768 bytes (the blob itself is never read)
    [lane]  CreateFeature(NVSDK_NGX_Feature_Reserved18) via the snippet -> 0x00000001 (...) handle=0x... 640x360 elapsed=1ms
    PCAB-RESULT mode=single valid=YES control=OK core_init=0x... feature=0x... desc_status=0 ...
"""

import re

# ---------------------------------------------------------------------------
# The statuses and results this investigation is about.
# ---------------------------------------------------------------------------

NVAPI_OK = 0
NVAPI_INVALID_POINTER = -14

NGX_SUCCESS = 0x00000001
NGX_FAIL_PLATFORM_ERROR = 0xBAD00002

#: The CreateCuModule blob size a working NeuralScreen run supplies. It is part
#: of the SINGLE reference gate because a run that supplies a different blob is
#: not running the same network.
REFERENCE_BLOB_SIZE = 3944768

NOT_OBSERVED = None


def _hex_to_int(text):
    try:
        return int(text, 16)
    except (TypeError, ValueError):
        return None


def _dec_to_int(text):
    try:
        return int(text, 10)
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------------------
# Shared: one observed private call.
# ---------------------------------------------------------------------------

class PrivateCall(object):
    """One invocation of GetCudaIndependentDescriptorObject or CreateCuModule."""

    def __init__(self, fn, seq, probe):
        self.fn = fn
        self.seq = seq
        self.probe = probe            # NULL-argument capability probe
        self.status = NOT_OBSERVED
        self.handle = NOT_OBSERVED    # returned_handle / returned module handle
        self.blob_size = NOT_OBSERVED
        self.device = NOT_OBSERVED
        self.desc_ptr = NOT_OBSERVED
        self.desc_type = NOT_OBSERVED
        self.struct_size_in = NOT_OBSERVED
        self.struct_size_out = NOT_OBSERVED
        self.match = NOT_OBSERVED
        self.scope = NOT_OBSERVED
        #: Why a handle was not read, when the callee said so. None means the
        #: question never came up.
        self.handle_note = None

    def as_dict(self):
        return {
            "fn": self.fn, "seq": self.seq, "probe": self.probe,
            "status": self.status, "handle": self.handle,
            "blob_size": self.blob_size, "device": self.device,
            "desc_ptr": self.desc_ptr, "desc_type": self.desc_type,
            "struct_size_in": self.struct_size_in,
            "struct_size_out": self.struct_size_out,
            "match": self.match, "scope": self.scope,
            "handle_note": self.handle_note,
        }


def _is_null_pointer(text):
    """True only for a printed null pointer.

    `%p` on MSVC prints sixteen zeros; the diagnostic prints the literal
    `(null)` in the places where it formats a null deliberately. Both are
    accepted, and nothing else is - a truncated or unparsed value must not be
    mistaken for "no pointer", because that is what classifies a probe.
    """
    if text in ("(null)", "0x0", "0x0000000000000000"):
        return True
    return bool(re.fullmatch(r"0+", text or ""))


# ---------------------------------------------------------------------------
# ReShade.log - the game-side log.
# ---------------------------------------------------------------------------

#: `[MGPU][P1.0c] Init: result=0x...` - the CORE form. The `which` field is
#: pinned to the four shapes the add-on actually emits, because a looser pattern
#: also matches `[MGPU][P1.0c] CreateFeature(Reserved18) ...: result=0x...` and
#: would silently report a CreateFeature result as a P1 Init result.
_RE_NGX_INIT = re.compile(
    r"\[MGPU\]\[(?P<phase>P1\.0c|P4\.1)\]\s+"
    r"(?P<which>(?:snippet\s+)?Init(?:_Ext)?(?:\s*\(FALLBACK[^)]*\))?):\s*"
    r"result=0x(?P<result>[0-9A-Fa-f]{8})")

#: The `via` field is not a fixed string (it names the module that answered), so
#: everything between the feature name and `result=` is skipped non-greedily
#: rather than assumed to contain no colon.
_RE_P1_CREATE = re.compile(
    r"\[MGPU\]\[P1\.0c\]\s+CreateFeature\(Reserved18\).*?"
    r"result=0x(?P<result>[0-9A-Fa-f]{8})\s*\((?P<name>[^)]*)\)\s*"
    r"handle=0x(?P<handle>[0-9A-Fa-f]+)")

_RE_P4_CREATE = re.compile(
    r"\[MGPU\]\[P4\.1\]\s+CreateFeature\(Reserved18\)\s+handle\s+(?P<i>\d+)/(?P<n>\d+)\s+"
    r"\d+x\d+\s+fmt=(?P<fmt>-?\d+):\s*result=0x(?P<result>[0-9A-Fa-f]{8})\s*"
    r"\((?P<name>[^)]*)\)\s*handle=0x(?P<handle>[0-9A-Fa-f]+)")

_RE_CUDA_DESC_HEAD = re.compile(
    r"\[CUDADIAG\]\s+GetCudaIndependentDescriptorObject\s+seq=(?P<seq>\d+)\s+"
    r"tid=(?P<tid>\d+)\s+scope=(?P<scope>\S+)\s+mismatch_seen=(?P<mm>\S+)\s+"
    r"pParams=(?P<params>\S+)")

_RE_CUDA_CU_HEAD = re.compile(
    r"\[CUDADIAG\]\s+CreateCuModule\s+seq=(?P<seq>\d+)\s+"
    r"tid=(?P<tid>\d+)\s+scope=(?P<scope>\S+)\s+mismatch_seen=(?P<mm>\S+)\s+"
    r"pBlob=(?P<blob>\S+)\s+size=(?P<size>\d+)\s+phModule=(?P<ph>\S+)")

_RE_CUDA_STATUS = re.compile(
    r"\[CUDADIAG\]\s+(?P<fn>GetCudaIndependentDescriptorObject|CreateCuModule)\s+"
    r"status=(?P<status>-?\d+)\s+\(0x(?P<status_hex>[0-9A-Fa-f]{8})\)")

_RE_CUDA_SIZES = re.compile(
    r"\[CUDADIAG\]\s+structSizeIn=(?P<sin>\d+)\s+structSizeOut=(?P<sout>\d+)")
_RE_CUDA_TYPE = re.compile(r"\[CUDADIAG\]\s+type=(?P<t>-?\d+)\s+\((?P<tn>[^)]*)\)")
_RE_CUDA_DESCPTR = re.compile(r"\[CUDADIAG\]\s+desc\.ptr=0x(?P<ptr>[0-9A-Fa-f]+)")
_RE_CUDA_RET_HANDLE = re.compile(r"\[CUDADIAG\]\s+returned handle=0x(?P<h>[0-9A-Fa-f]+)")
_RE_CUDA_RET_MODULE = re.compile(r"\[CUDADIAG\]\s+returned module handle=(?P<h>\S+)")
_RE_CUDA_HANDLE_NOT_READ = re.compile(
    r"\[CUDADIAG\]\s+returned (handle|module handle) NOT READ")
_RE_CUDA_DEVICE = re.compile(
    r"\[CUDADIAG\]\s+(?P<fn>GetCudaIndependentDescriptorObject|CreateCuModule)\s+"
    r"(?P<phase>before|after)\s+pDevice=(?P<dev>\S+)")
_RE_CUDA_ARMED = re.compile(r"\[CUDADIAG\]\s+armed\s+(?P<what>\S+)\s+real=(?P<real>\S+)")
_RE_APPID = re.compile(r"\[APPID\]\s+(?P<body>.*)$")
_RE_RESHADENATIVE = re.compile(r"\[RESHADENATIVE\]\s+(?P<body>.*)$")

_RE_P1_DESC_HEAD = re.compile(
    r"\[cuda\]\s+#(?P<n>\d+)\s+GetCudaIndependentDescriptorObject\s+0x0DDAC234\s+"
    r"(?P<kind>REAL|CAPABILITY-PROBE)\s+pDevice=(?P<dev>\S+)\s+type=(?P<t>-?\d+)\s+"
    r"desc\.ptr=0x(?P<ptr>[0-9A-Fa-f]+)\s+structSizeIn=(?P<sin>\d+)\s+"
    r"structSizeOut_before=(?P<sout>\d+)\s+match=(?P<m>\S+)")
_RE_P1_CU_HEAD = re.compile(
    r"\[cuda\]\s+#(?P<n>\d+)\s+CreateCuModule\s+0xAD1A677D\s+"
    r"(?P<kind>REAL|CAPABILITY-PROBE)\s+pDevice=(?P<dev>\S+)\s+pBlob=(?P<blob>\S+)\s+"
    r"\(pointer only; never dereferenced\)\s+size=(?P<size>\d+)\s+phModule=(?P<ph>\S+)\s+"
    r"match=(?P<m>\S+)")
_RE_P1_STATUS = re.compile(
    r"\[cuda\]\s+#(?P<n>\d+)\s+(?P<kind>REAL|probe)\s+status=(?P<status>-?\d+)\s+"
    r"\(0x(?P<hex>[0-9A-Fa-f]{8})\)(?P<rest>.*)$")
_RE_P1_DESC_DETAIL = re.compile(
    r"structSizeOut_after=(?P<sout>\d+)\s+returned_handle=0x(?P<h>[0-9A-Fa-f]+)")
_RE_P1_CU_DETAIL = re.compile(r"module_handle=0x(?P<h>[0-9A-Fa-f]+)")
_RE_P1_BLOB = re.compile(
    r"\[cuda\]\s+first CreateCuModule blob size = (?P<size>\d+) bytes")
_RE_P1_FEATURE = re.compile(
    r"\[lane\]\s+CreateFeature\(NVSDK_NGX_Feature_Reserved18\)\s+via\s+[^>]*->\s*"
    r"0x(?P<result>[0-9A-Fa-f]{8})\s*\((?P<name>[^)]*)\)\s*handle=0x(?P<h>[0-9A-Fa-f]+)")
_RE_PCAB_RESULT = re.compile(r"PCAB-RESULT\s+(?P<body>.+)$")


class ReShadeLogParser(object):
    """Incremental parser for one ReShade.log.

    `feed(line)` may be called repeatedly as the file grows; the observation
    state accumulates. Nothing is ever overwritten by an absent value, so a
    field once observed stays observed and a field never observed stays None.
    """

    def __init__(self):
        self.p1_core_init = NOT_OBSERVED        # result code, or None
        self.p1_core_init_which = NOT_OBSERVED
        self.p4_core_init = NOT_OBSERVED
        self.p1_snippet_init = NOT_OBSERVED
        self.p1_snippet_init_which = NOT_OBSERVED

        self.p1_create_feature = NOT_OBSERVED
        self.p1_create_handle = NOT_OBSERVED
        self.p1_create_via = NOT_OBSERVED
        self.p4_create_feature = NOT_OBSERVED
        self.p4_create_handle = NOT_OBSERVED

        self.descriptor_calls = []
        self.cumodule_calls = []

        self.appid_lines = []
        self.reshadenative_lines = []
        self.cudadiag_variant = NOT_OBSERVED
        self.cudadiag_armed = []
        self.cudadiag_expect_note = NOT_OBSERVED
        self.ngx_armed_notes = []

        self._current = None
        self._saw_any_cudadiag = False

    # -- incremental entry point ------------------------------------------
    def feed(self, line):
        line = line.rstrip("\r\n")
        if not line:
            return

        m = _RE_CUDA_DESC_HEAD.search(line)
        if m:
            self._saw_any_cudadiag = True
            call = PrivateCall("GetCudaIndependentDescriptorObject",
                               _dec_to_int(m.group("seq")),
                               probe=_is_null_pointer(m.group("params")))
            call.scope = m.group("scope")
            # The device is not on this line: `log_device` prints it separately,
            # and that line is what sets call.device.
            self.descriptor_calls.append(call)
            self._current = call
            return

        m = _RE_CUDA_CU_HEAD.search(line)
        if m:
            self._saw_any_cudadiag = True
            null_args = (_is_null_pointer(m.group("blob")) and
                         _is_null_pointer(m.group("ph")) and
                         _dec_to_int(m.group("size")) == 0)
            call = PrivateCall("CreateCuModule",
                               _dec_to_int(m.group("seq")),
                               probe=null_args)
            call.scope = m.group("scope")
            call.blob_size = _dec_to_int(m.group("size"))
            self.cumodule_calls.append(call)
            self._current = call
            return

        m = _RE_CUDA_DEVICE.search(line)
        if m:
            dev = m.group("dev")
            if self._current is not None and self._current.fn == m.group("fn"):
                if not _is_null_pointer(dev):
                    self._current.device = dev
            return

        m = _RE_CUDA_SIZES.search(line)
        if m and self._current is not None:
            self._current.struct_size_in = _dec_to_int(m.group("sin"))
            self._current.struct_size_out = _dec_to_int(m.group("sout"))
            return

        m = _RE_CUDA_TYPE.search(line)
        if m and self._current is not None:
            self._current.desc_type = m.group("tn")
            return

        m = _RE_CUDA_DESCPTR.search(line)
        if m and self._current is not None:
            self._current.desc_ptr = _hex_to_int(m.group("ptr"))
            return

        m = _RE_CUDA_STATUS.search(line)
        if m:
            if self._current is not None and self._current.fn == m.group("fn"):
                self._current.status = _dec_to_int(m.group("status"))
            else:
                # A status with no header of its own. It is still a real
                # observation, so it is recorded on a synthetic call rather than
                # dropped - dropping it would make an observed failure vanish.
                call = PrivateCall(m.group("fn"), NOT_OBSERVED, probe=False)
                call.status = _dec_to_int(m.group("status"))
                (self.descriptor_calls if m.group("fn").startswith("GetCuda")
                 else self.cumodule_calls).append(call)
                self._current = call
            # _current is deliberately NOT cleared: the callee prints the __out
            # value on the line AFTER the status, so a handle belongs to the call
            # whose status was just read. Clearing here was a real bug - it
            # silently dropped every returned handle.
            return

        m = _RE_CUDA_RET_HANDLE.search(line)
        if m:
            # Only a SUCCESSFUL call has its handle read; the C++ never prints
            # one otherwise. Requiring the status here means a handle can never
            # be attached to a failed call.
            if self._current is not None and self._current.status == 0:
                self._current.handle = _hex_to_int(m.group("h"))
            return

        m = _RE_CUDA_RET_MODULE.search(line)
        if m:
            if self._current is not None and self._current.status == 0:
                h = m.group("h")
                # `%p` prints sixteen zeros when the handle came back null; that
                # is an OBSERVED null, which is different from not read.
                self._current.handle = 0 if _is_null_pointer(h) else _hex_to_int(h)
            return

        if _RE_CUDA_HANDLE_NOT_READ.search(line):
            # Deliberately left as NOT_OBSERVED, but the reason is recorded so
            # "the callee did not write it and we did not read it" is
            # distinguishable from "the line never appeared".
            if self._current is not None:
                self._current.handle_note = line.split("[CUDADIAG]", 1)[1].strip()
            return

        m = _RE_CUDA_ARMED.search(line)
        if m:
            self.cudadiag_armed.append(m.group("what"))
            return

        if line.startswith("[CUDADIAG] variant="):
            self.cudadiag_variant = line.split("variant=", 1)[1].split()[0]
            return

        if line.startswith("[CUDADIAG] expect an"):
            self.cudadiag_expect_note = line
            return

        if line.startswith("[MGPU][") and "armed" in line and "nvapi_QueryInterface" not in line:
            self.ngx_armed_notes.append(line)

        m = _RE_NGX_INIT.search(line)
        if m:
            phase = m.group("phase")
            which = m.group("which").strip()
            result = _hex_to_int(m.group("result"))
            low = which.lower()
            if phase == "P4.1":
                # The P4.1 stream logs exactly one core Init line.
                self.p4_core_init = result
            elif low.startswith("snippet"):
                self.p1_snippet_init = result
                self.p1_snippet_init_which = which
            else:
                # `Init` or the `Init_Ext (FALLBACK ...)` form. Both are the
                # core Reserved18 session, which is what P1 Init means here.
                self.p1_core_init = result
                self.p1_core_init_which = which
            return

        m = _RE_P1_CREATE.search(line)
        if m:
            self.p1_create_feature = _hex_to_int(m.group("result"))
            self.p1_create_handle = _hex_to_int(m.group("handle"))
            # `via` is not captured: it names the module that answered and is not
            # an observation this investigation uses.
            return

        m = _RE_P4_CREATE.search(line)
        if m:
            self.p4_create_feature = _hex_to_int(m.group("result"))
            self.p4_create_handle = _hex_to_int(m.group("handle"))
            return

        m = _RE_APPID.search(line)
        if m:
            self.appid_lines.append(m.group("body"))
            return

        m = _RE_RESHADENATIVE.search(line)
        if m:
            self.reshadenative_lines.append(m.group("body"))
            return

    # -- derived views ----------------------------------------------------
    @property
    def saw_cudadiag(self):
        return self._saw_any_cudadiag

    def first_real(self, calls):
        """The FIRST REAL call, or None. Probes never occupy this slot."""
        for c in calls:
            if not c.probe:
                return c
        return None

    def first_real_in_scope(self, calls, scope):
        """The first real call CUDADIAG attributed to one phase.

        CUDADIAG labels every call with the scope it happened in - `P1.0c` for
        the startup probe, `P4.1` for the real stream, `outside` for neither. So
        the phase a call belongs to is READ from the log, not guessed from the
        order the lines appeared in.
        """
        for c in calls:
            if not c.probe and c.scope == scope:
                return c
        return None

    def first_real_status(self, calls):
        c = self.first_real(calls)
        return NOT_OBSERVED if c is None else c.status

    def probe_count(self, calls):
        return sum(1 for c in calls if c.probe)

    @property
    def descriptor_real(self):
        return self.first_real(self.descriptor_calls)

    @property
    def cumodule_real(self):
        return self.first_real(self.cumodule_calls)

    def observation_summary(self):
        """Everything observed here, as plain data. None means NOT_OBSERVED."""
        d = self.descriptor_real
        c = self.cumodule_real
        return {
            "p1_core_init": self.p1_core_init,
            "p1_core_init_which": self.p1_core_init_which,
            "p1_snippet_init": self.p1_snippet_init,
            "p1_snippet_init_which": self.p1_snippet_init_which,
            "p4_core_init": self.p4_core_init,
            "p1_create_feature": self.p1_create_feature,
            "p1_create_handle": self.p1_create_handle,
            "p4_create_feature": self.p4_create_feature,
            "p4_create_handle": self.p4_create_handle,
            "descriptor_call_count": len(self.descriptor_calls),
            "descriptor_probe_count": self.probe_count(self.descriptor_calls),
            "descriptor_real_status": NOT_OBSERVED if d is None else d.status,
            "descriptor_real_handle": NOT_OBSERVED if d is None else d.handle,
            "descriptor_desc_ptr": NOT_OBSERVED if d is None else d.desc_ptr,
            "descriptor_device": NOT_OBSERVED if d is None else d.device,
            "cumodule_call_count": len(self.cumodule_calls),
            "cumodule_probe_count": self.probe_count(self.cumodule_calls),
            "cumodule_real_status": NOT_OBSERVED if c is None else c.status,
            "cumodule_real_handle": NOT_OBSERVED if c is None else c.handle,
            "cumodule_blob_size": NOT_OBSERVED if c is None else c.blob_size,
            "cudadiag_variant": self.cudadiag_variant,
            "cudadiag_armed": list(self.cudadiag_armed),
            "appid_lines": list(self.appid_lines),
            "reshadenative_lines": list(self.reshadenative_lines),
            "descriptor_calls": [x.as_dict() for x in self.descriptor_calls],
            "cumodule_calls": [x.as_dict() for x in self.cumodule_calls],
        }


    def phase_observations(self):
        """The p1 / p4 split, attributed by CUDADIAG's OWN scope label.

        CUDADIAG stamps every call with the scope it happened in, so which phase
        a call belongs to is read from the log rather than guessed from the
        order the lines appeared in. A phase field is None when no real call
        carried that phase's scope: it is never copied from the other phase, and
        an unscoped call is reported as unscoped rather than assigned.
        """
        d1 = self.first_real_in_scope(self.descriptor_calls, "P1.0c")
        d4 = self.first_real_in_scope(self.descriptor_calls, "P4.1")
        c1 = self.first_real_in_scope(self.cumodule_calls, "P1.0c")
        c4 = self.first_real_in_scope(self.cumodule_calls, "P4.1")

        def view(c):
            if c is None:
                return {"observed": False, "status": None, "handle": None,
                        "blob_size": None, "handle_note": None}
            return {"observed": True, "status": c.status, "handle": c.handle,
                    "blob_size": c.blob_size, "handle_note": c.handle_note}

        v1d, v4d, v1c, v4c = view(d1), view(d4), view(c1), view(c4)
        return {
            "p1": {"init": self.p1_core_init, "descriptor": v1d["status"],
                   "cumodule": v1c["status"], "create_feature": self.p1_create_feature},
            "p4": {"init": self.p4_core_init, "descriptor": v4d["status"],
                   "cumodule": v4c["status"], "create_feature": self.p4_create_feature},
            "detail": {"p1_descriptor": v1d, "p4_descriptor": v4d,
                       "p1_cumodule": v1c, "p4_cumodule": v4c},
        }


def parse_reshade_log(text):
    p = ReShadeLogParser()
    for line in text.splitlines():
        p.feed(line)
    return p

# ---------------------------------------------------------------------------
# PROCESSCONTEXT arm logs.
# ---------------------------------------------------------------------------

class ProcessContextArmParser(object):
    """Parser for one `context-<mode>.log`."""

    def __init__(self, mode):
        self.mode = mode
        self.valid = NOT_OBSERVED          # from PCAB-RESULT valid=YES/NO
        self.control = NOT_OBSERVED        # OK / FAIL / NOT_RUN
        self.reason = NOT_OBSERVED
        self.feature = NOT_OBSERVED
        # The NGX entry-point results the machine-readable line carries. They are
        # recorded because they say whether the lane ran at all; they are not the
        # reference gate, which is about the two private calls.
        self.core_init = NOT_OBSERVED
        self.caps = NOT_OBSERVED
        self.snip_init = NOT_OBSERVED
        self.populate = NOT_OBSERVED
        self.result_desc_status = NOT_OBSERVED
        self.result_cu_status = NOT_OBSERVED
        self.descriptor_calls = []
        self.cumodule_calls = []
        self.blob_size = NOT_OBSERVED
        self.saw_result_line = False
        #: Lines whose call could not be attributed to either private function.
        #: A non-empty list fails the arm: an unattributed line means the log is
        #: not the shape this parser was written against.
        self.unattributed = []
        self._current = None

    def feed(self, line):
        line = line.rstrip("\r\n")
        if not line:
            return

        m = _RE_PCAB_RESULT.search(line)
        if m:
            self.saw_result_line = True
            for token in m.group("body").split():
                if "=" not in token:
                    continue
                k, v = token.split("=", 1)
                if k == "valid":
                    self.valid = (v == "YES")
                elif k == "control":
                    self.control = v
                elif k == "reason":
                    self.reason = v
                elif k == "feature":
                    self.feature = _hex_to_int(v)
                elif k == "core_init":
                    self.core_init = _hex_to_int(v)
                elif k == "caps":
                    self.caps = _hex_to_int(v)
                elif k == "snip_init":
                    self.snip_init = _hex_to_int(v)
                elif k == "populate":
                    self.populate = _hex_to_int(v)
                elif k == "desc_status":
                    self.result_desc_status = NOT_OBSERVED if v == "NA" else _dec_to_int(v)
                elif k == "cu_status":
                    self.result_cu_status = NOT_OBSERVED if v == "NA" else _dec_to_int(v)
            return

        m = _RE_P1_DESC_HEAD.search(line)
        if m:
            call = PrivateCall("GetCudaIndependentDescriptorObject",
                               _dec_to_int(m.group("n")),
                               probe=(m.group("kind") == "CAPABILITY-PROBE"))
            call.device = m.group("dev")
            call.desc_ptr = _hex_to_int(m.group("ptr"))
            call.struct_size_in = _dec_to_int(m.group("sin"))
            call.struct_size_out = _dec_to_int(m.group("sout"))
            call.match = m.group("m")
            self.descriptor_calls.append(call)
            self._current = call
            return

        m = _RE_P1_CU_HEAD.search(line)
        if m:
            call = PrivateCall("CreateCuModule",
                               _dec_to_int(m.group("n")),
                               probe=(m.group("kind") == "CAPABILITY-PROBE"))
            call.device = m.group("dev")
            call.blob_size = _dec_to_int(m.group("size"))
            call.match = m.group("m")
            self.cumodule_calls.append(call)
            self._current = call
            return

        m = _RE_P1_STATUS.search(line)
        if m:
            n = _dec_to_int(m.group("n"))
            probe = (m.group("kind") == "probe")
            rest = m.group("rest") or ""
            status = _dec_to_int(m.group("status"))

            target = None
            if self._current is not None and self._current.seq == n:
                target = self._current
            else:
                # The header for this line is not the most recent one - only
                # reachable if two arms interleave, which they do not today. The
                # line is attributed by the fields it carries, and a line that
                # carries none (a probe) is recorded as UNATTRIBUTED rather than
                # guessed into one of the two calls.
                if "module_handle" in rest:
                    target = PrivateCall("CreateCuModule", n, probe)
                    self.cumodule_calls.append(target)
                elif "returned_handle" in rest or "structSizeOut_after" in rest:
                    target = PrivateCall("GetCudaIndependentDescriptorObject", n, probe)
                    self.descriptor_calls.append(target)
                else:
                    self.unattributed.append(line)
                    self._current = None
                    return

            target.probe = probe
            target.status = status

            # A FAILED call prints `returned_handle=0x0 NOT READ: ...`. That zero
            # is the placeholder the wrapper passed in, not a handle the callee
            # wrote, so it must NOT become an observed handle. structSizeOut is a
            # different matter: it is the caller's own declared size and reading
            # it is what the version guard is for.
            not_read = "NOT READ" in rest
            dm = _RE_P1_DESC_DETAIL.search(rest)
            if dm:
                target.struct_size_out = _dec_to_int(dm.group("sout"))
                if not not_read:
                    target.handle = _hex_to_int(dm.group("h"))
            cm = _RE_P1_CU_DETAIL.search(rest)
            if cm and not not_read:
                target.handle = _hex_to_int(cm.group("h"))
            if not_read:
                target.handle_note = rest[rest.index("NOT READ"):].strip()
            self._current = None
            return

        m = _RE_P1_BLOB.search(line)
        if m:
            self.blob_size = _dec_to_int(m.group("size"))
            return

        m = _RE_P1_FEATURE.search(line)
        if m:
            self.feature = _hex_to_int(m.group("result"))
            return

    def first_real(self, calls):
        for c in calls:
            if not c.probe:
                return c
        return None

    @property
    def descriptor_real(self):
        return self.first_real(self.descriptor_calls)

    @property
    def cumodule_real(self):
        return self.first_real(self.cumodule_calls)

    def cross_check(self):
        """Compare the machine-readable line against the detailed [cuda] lines.

        Two independent descriptions of the same call. A disagreement means one
        of them is wrong and this arm cannot be read, so it is reported rather
        than resolved.
        """
        problems = []
        d = self.descriptor_real
        c = self.cumodule_real
        detail_desc = NOT_OBSERVED if d is None else d.status
        detail_cu = NOT_OBSERVED if c is None else c.status
        if self.unattributed:
            problems.append("%d unattributed call line(s)" % len(self.unattributed))
        if self.result_desc_status is not NOT_OBSERVED and detail_desc is not NOT_OBSERVED:
            if self.result_desc_status != detail_desc:
                problems.append("descriptor status: PCAB-RESULT=%s detailed=%s"
                                % (self.result_desc_status, detail_desc))
        if self.result_cu_status is not NOT_OBSERVED and detail_cu is not NOT_OBSERVED:
            if self.result_cu_status != detail_cu:
                problems.append("cumodule status: PCAB-RESULT=%s detailed=%s"
                                % (self.result_cu_status, detail_cu))
        if self.blob_size is not NOT_OBSERVED and c is not NOT_OBSERVED:
            if c.blob_size is not NOT_OBSERVED and c.blob_size != self.blob_size:
                problems.append("blob size: summary=%s call=%s"
                                % (self.blob_size, c.blob_size))
        return problems

    def summary(self):
        d = self.descriptor_real
        c = self.cumodule_real
        return {
            "mode": self.mode,
            "valid": self.valid,
            "control": self.control,
            "reason": self.reason,
            "feature": self.feature,
            "core_init": self.core_init,
            "caps": self.caps,
            "snip_init": self.snip_init,
            "populate": self.populate,
            "descriptor_status": NOT_OBSERVED if d is None else d.status,
            "descriptor_handle": NOT_OBSERVED if d is None else d.handle,
            "cumodule_status": NOT_OBSERVED if c is None else c.status,
            "cumodule_handle": NOT_OBSERVED if c is None else c.handle,
            "blob_size": self.blob_size,
            "descriptor_probes": sum(1 for x in self.descriptor_calls if x.probe),
            "cumodule_probes": sum(1 for x in self.cumodule_calls if x.probe),
            "parse_conflicts": self.cross_check(),
        }


def parse_processcontext_log(text, mode):
    p = ProcessContextArmParser(mode)
    for line in text.splitlines():
        p.feed(line)
    return p
