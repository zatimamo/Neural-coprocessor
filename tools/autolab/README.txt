================================================================================
MGPU-AutoLab
A local, deterministic experiment orchestrator for the MGPU / DLSS-NR
investigation.
================================================================================

WHAT IT IS

One command - RUN-AUTOLAB.cmd - executes the complete predefined experiment tree
and stops on the first of these:

    1. a decisive causal result,
    2. a run that is invalid,
    3. the experiment tree exhausted,
    4. an unexpected or mixed state.

It does the work a human was doing by hand: triggering GitHub builds,
downloading artifacts, verifying hashes, deploying add-ons, clearing logs,
launching Cyberpunk, waiting for the failure, grepping ReShade.log, archiving
results, and deciding which already-defined experiment comes next.

WHAT IT IS NOT

It is an automation project, NOT another hypothesis. The decisions live in
experiments.py as DATA. The controller has no branch that could express a new
hypothesis, and a result matching no declared rule is UNEXPECTED_MIXED_STATE,
which stops the run. No model - and no heuristic - chooses what to try next.


FILES

    autolab.py          the controller: CLI, preflight, the graph walk, the
                        report, --list / --parser-tests / --dry-run
    config.yaml         every path, timeout and gate, and the one safety switch
    experiments.py      the workflows, the gates, and THE DECISION GRAPH AS DATA
    github_actions.py   trigger / locate the exact run / wait / download / hash,
                        and the artifact cache keyed by commit SHA + experiment
    deployment.py       backup, deploy, verify, RESTORE
    cyberpunk.py        process control, log tailing, CloseMainWindow, archive
    log_parser.py       ReShade.log and PROCESSCONTEXT arm-log parsers
    processcontext.py   the four-arm standalone diagnostic, run as four fresh
                        processes
    report.py           results/<timestamp>/ - summary, hashes, timeline,
                        environment, logs
    RUN-AUTOLAB.cmd     the one command
    fixtures/           synthetic logs: the parser and graph tests run on these


USAGE

    RUN-AUTOLAB.cmd                     the whole tree
    RUN-AUTOLAB.cmd --list              the graph and every rule, then exit
    RUN-AUTOLAB.cmd --parser-tests      fixtures only; no hardware, no network
    RUN-AUTOLAB.cmd --dry-run           synthetic logs through the real code
    RUN-AUTOLAB.cmd --preflight-only    every check; nothing built, deployed
                                        or launched
    RUN-AUTOLAB.cmd --only PROCESSCONTEXT
    RUN-AUTOLAB.cmd --no-cache          force a fresh build

Exit codes: 0 a verdict was reached, 1 an error, 2 a config or graph error,
3 restoration failed (act on that immediately), 130 interrupted.


THE SAFETY GATE

Cyberpunk is NOT launched while safety.allow_game_launch is false in config.yaml.
That is why AutoLab can be developed and tested against fixtures without a game
launch. Set it to true - or pass --allow-game-launch - when you are ready.

The gate is checked inside cyberpunk.launch(), not only in the CLI, so no code
path can bypass it.


WHAT IT WRITES, AND WHAT IT WILL NOT TOUCH

Written in the game directory:

    paths.addon_name          the add-on under test, and nothing else
    paths.reshade_log         deleted before each game run

Everything else in the game directory is read-only to AutoLab. In particular the
NR runtime (paths.nr_dll) is hashed and verified, and NEVER replaced: it is the
fixed instrument the whole investigation is measured against.

TWO RUNTIMES, AND WHY

    paths.nr_dll                 the copy the GAME loads, relative to install_dir
    paths.neuralscreen_nr_dll    the NEURALSCREEN REFERENCE runtime, absolute

The PROCESSCONTEXT node runs THE REFERENCE LANE, so it uses the second one and
passes it through untouched. It is not copied into AutoLab's staging directory,
because the diagnostic derives the NGX data path as dirname(--nr-dll) and that
directory has to be the one NeuralScreen itself hands NGX. Both are
hash-verified; a wrong or missing one stops the run in preflight, before CI is
asked to build anything.

PROCESSCONTEXT, THE FIRST LIVE NODE

    locate paths.neuralscreen_nr_dll        preflight, and again before staging
    verify DCC0DC...D36F                    both copies, both times
    launch RUN-CONTEXT-AB.cmd               once for single, once for the rest
    wait for completion                     bounded by the configured timeout
    parse the PCAB-RESULT records           one per arm
    enforce SINGLE as the reference gate    if it fails the second launch never
                                            happens; the result records the arms
                                            that were not launched
    classify with the fixed A/B/C/D/E rules experiments.decide
    archive the arm logs                    logs/processcontext-<mode>.log
    write summary.json and summary.txt      report.py
    stop at the predefined verdict          the graph

AutoLab calls the artifact's OWN launcher rather than re-implementing the
four-arm sequence, and gives it an explicit arm list - which is how SINGLE can
stop the other three before they are started. SINGLE therefore runs exactly
once, and no game is launched for this node.

The original add-on is backed up and its hash verified BEFORE anything is
written, and restored from a finally block - so normal completion, Ctrl+C, a
Python exception, a timeout and an invalid result all restore. Restoration also
skips the write entirely when the installed file already matches the backup,
which is what makes --preflight-only completely read-only as far as the game
directory is concerned.


THE REFERENCE GATE

PROCESSCONTEXT runs four arms, each a fresh process, and SINGLE is the control.
If SINGLE does not reproduce the working result -

    descriptor 0, CuModule 0, blob 3944768, Reserved18 Success, handle non-zero

- then no other arm is readable and the run stops with
PROCESSCONTEXT_REFERENCE_INVALID. The gate is declared as data in
experiments.PROCESSCONTEXT_REFERENCE_GATE and evaluated, not restated as prose.


READING A RESULT

A field that was not observed is null, never zero and never a default. A
NULL-argument capability probe returning -14 is recorded separately and can
never become a real status. A run whose P1.0c or P4.1 Init is not Success is
INVALID, and a timeout is INVALID/TIMEOUT - never a guessed result.


TESTS

    RUN-AUTOLAB.cmd --parser-tests

runs the fixture parser tests and the graph tests, including negative controls
that mutate a fixture so the property under test must flip. If a control stops
flipping, the corresponding test has stopped testing anything.

    RUN-AUTOLAB.cmd --dry-run

puts the fixtures through the SAME parse and decide functions the live run uses,
walking every branch of the graph, and writes a real results tree. It builds
nothing, deploys nothing and never launches the game.


A NOTE ON THE GITHUB PATH

AutoLab talks to the REST API with urllib from the standard library, so it works
with no extra installs. gh is used when it is present, but only as a token
source and for `gh auth status` - there is one code path that performs the API
calls, so the two cannot disagree about which run was triggered. Without gh the
token comes from GH_TOKEN / GITHUB_TOKEN, or from the same credential helper git
already uses for this repository.

An artifact is always tied to the exact run that produced it: AutoLab dispatches
the workflow, records the run id, waits for that run, requires conclusion =
success, and downloads the artifact of that run id. "Latest artifact" is never
accepted.
