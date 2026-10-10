#!/usr/bin/env python3
"""Unit tests for tools/configure.py.

This is the largest hole the project had. `configure.py` is what turns
raylib_multiplatform.toml into every build input there is — the CMake variables,
the Gradle properties, the Android manifest, the XcodeGen spec, the version
numbers. Everything else in tests/ checks the code that runs *after* it, and
until now nothing checked the thing that decides what gets built at all.

The one that matters most is resolve_version(). Its versionCode arithmetic —
major*1_000_000 + minor*1_000 + patch — has a **permanent consequence**: Google
Play refuses an upload whose versionCode is not higher than the last one, and
there is no way to lower it afterwards. An off-by-one there is not a bug you fix
next release; it is a listing you cannot upload to.

Run it with `rmp test` (part of the default set) or on its own:

    python3 tests/configure_test.py
    python3 tests/configure_test.py -v ConfigureVersionTest

Standard library only, on purpose: `rmp test` must not need a pip install, and
this has to run on a Windows runner and inside the pinned container alike.
"""

from __future__ import annotations

import ast
import bisect
import hashlib
import contextlib
import copy
import re
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def load_configure():
    """Import tools/configure.py as a module, by path.

    It is a script, not a package, and it must stay that way — the CMake hook
    runs it directly. So the test imports it the way a test should: without
    asking the thing under test to change shape for the test's convenience.
    """
    spec = importlib.util.spec_from_file_location("rmp_configure", REPO / "tools" / "configure.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["rmp_configure"] = module
    spec.loader.exec_module(module)
    return module


cfgmod = load_configure()


# What configure.py generates into the checkout. No test here may write or
# delete any of it: test_off_means_no_files_at_all once ran gen_licenses() with
# only write() stubbed, and its unlink() deleted the real
# cmake/generated/LICENSES.txt and the Android one every time the suite ran --
# the next package step shipped no notice, and nothing said so. A test points
# configure.py at a temporary tree instead (licence_files_in, repo_at), and
# this compares the generated files before and after the whole module.
GENERATED = (REPO / "cmake" / "generated", REPO / "include" / "rmp" / "generated",
             REPO / "raymob" / "generated.properties", REPO / "ios" / "project.yml",
             REPO / "ios" / "Assets.xcassets")


def generated_listing() -> dict:
    """path -> sha256 of every generated file in the checkout."""
    out = {}
    for root in GENERATED:
        paths = [root] if root.is_file() else (sorted(root.rglob("*")) if root.is_dir() else [])
        for path in paths:
            if path.is_file():
                out[path.relative_to(REPO).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


_generated_before: dict = {}


def setUpModule():
    _generated_before.update(generated_listing())


def tearDownModule():
    after = generated_listing()
    gone = sorted(set(_generated_before) - set(after))
    made = sorted(set(after) - set(_generated_before))
    edited = sorted(p for p in set(after) & set(_generated_before)
                    if after[p] != _generated_before[p])
    if gone or made or edited:
        raise AssertionError(
            "the configure tests changed the checkout's generated files -- a test has to "
            f"point configure.py at a temporary tree.\n  deleted: {gone}\n  created: {made}\n"
            f"  rewritten: {edited}")


def base_config(**overrides) -> dict:
    """A valid configuration, with the example identifiers made real.

    DEFAULTS ships com.example.* on purpose, and validate() rejects those under
    --strict-release. Tests that are not about that rule start from something
    that would actually pass.
    """
    cfg = copy.deepcopy(cfgmod.DEFAULTS)
    cfg["android"]["application_id"] = "com.omardev.game"
    cfg["ios"]["bundle_id"] = "com.omardev.game"
    for key, value in overrides.items():
        section, _, field = key.partition("__")
        if field:
            cfg[section][field] = value
        elif isinstance(value, dict) and isinstance(cfg.get(section), dict):
            # MERGED, not replaced. A test that writes out a whole section by
            # hand goes stale the moment a key is added to it, and it goes stale
            # as a KeyError inside validate() rather than as a message about the
            # test — which happened twice, for [upx] max_size_mb and again for
            # [linux] glibc. Merging means a test says what it is testing and
            # inherits the rest.
            cfg[section] = {**cfg[section], **value}
        else:
            cfg[section] = value
    return cfg


@contextlib.contextmanager
def licence_files_in():
    """configure.py's LICENSE_FILES, pointed at a temporary tree laid out like
    the checkout (cmake/generated/...), which is yielded."""
    original = dict(cfgmod.LICENSE_FILES)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for family, path in original.items():
            cfgmod.LICENSE_FILES[family] = root / path.relative_to(REPO)
        try:
            yield root
        finally:
            cfgmod.LICENSE_FILES.clear()
            cfgmod.LICENSE_FILES.update(original)


@contextlib.contextmanager
def generated_header(cfg):
    """What gen_app_config() would write for `cfg`, without writing it.

    configure.py's write() is the one door every generated file goes through,
    so swapping it for a capture is enough to read the header back."""
    captured = {}
    original = cfgmod.write
    cfgmod.write = lambda path, content: captured.__setitem__(str(path), content)
    try:
        cfgmod.gen_app_config(cfg)
        (text,) = captured.values()
        yield text
    finally:
        cfgmod.write = original


@contextlib.contextmanager
def quiet():
    """configure.py warns on stderr. A test run should not be noisy."""
    with contextlib.redirect_stderr(io.StringIO()):
        yield


IN_BUILD_IMAGE = Path("/etc/raylib-build-image.json").is_file()


def require_yaml(case):
    """PyYAML, or a decision about why it is not here.

    These tests used to `skipTest("PyYAML not installed; the iOS job has it")`,
    and the iOS job does not run this file -- the LINT job does, inside the
    build image, which had no PyYAML either. So they were skipped everywhere
    and the skip read like a pass. The image ships python3-yaml now, and inside
    it a missing PyYAML is a failure: a gate that cannot fail is not a gate.

    On a laptop it stays a skip. Somebody who has just cloned the repository
    should not have to pip-install anything to run `rmp test`.
    """
    try:
        import yaml
    except ImportError:
        if IN_BUILD_IMAGE:
            case.fail("PyYAML is missing inside the build image, so this test cannot "
                      "run. It is what checks the generated Xcode spec parses at all. "
                      "Add python3-yaml to the image.")
        case.skipTest("PyYAML not installed (it ships in the build image, where "
                      "this is a failure rather than a skip)")
    return yaml


# ---------------------------------------------------------------------------


class ConfigureVersionTest(unittest.TestCase):
    """resolve_version(): the one with a consequence you cannot take back."""

    def setUp(self):
        self._saved = {k: os.environ.get(k) for k in ("GITHUB_REF_TYPE", "GITHUB_REF_NAME")}

    def tearDown(self):
        for key, value in self._saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    def at_tag(self, name: str):
        os.environ["GITHUB_REF_TYPE"] = "tag"
        os.environ["GITHUB_REF_NAME"] = name
        return cfgmod.resolve_version()

    def test_untagged_build_is_dev(self):
        os.environ.pop("GITHUB_REF_TYPE", None)
        os.environ.pop("GITHUB_REF_NAME", None)
        self.assertEqual(cfgmod.resolve_version(), ("0.0.0-dev", 1))

    def test_branch_push_is_dev(self):
        os.environ["GITHUB_REF_TYPE"] = "branch"
        os.environ["GITHUB_REF_NAME"] = "main"
        self.assertEqual(cfgmod.resolve_version(), ("0.0.0-dev", 1))

    def test_plain_versions(self):
        self.assertEqual(self.at_tag("v1.2.3"), ("1.2.3", 1_002_003))
        self.assertEqual(self.at_tag("v0.0.1"), ("0.0.1", 1))
        self.assertEqual(self.at_tag("v0.1.0"), ("0.1.0", 1_000))
        self.assertEqual(self.at_tag("v10.20.30"), ("10.20.30", 10_020_030))

    def test_the_arithmetic_has_no_off_by_one(self):
        """Each component lands in its own decimal window and nothing bleeds."""
        self.assertEqual(self.at_tag("v0.0.999")[1], 999)
        self.assertEqual(self.at_tag("v0.1.0")[1], 1_000)
        self.assertEqual(self.at_tag("v0.999.999")[1], 999_999)
        self.assertEqual(self.at_tag("v1.0.0")[1], 1_000_000)

    def test_version_codes_increase_with_the_version(self):
        """The invariant Play actually enforces, checked as a property.

        A versionCode that ever goes down is an upload that is refused forever.
        This is the test that would catch a change to the packing formula.
        """
        ordered = ["v0.0.1", "v0.0.2", "v0.0.999", "v0.1.0", "v0.1.1", "v0.999.999",
                   "v1.0.0", "v1.0.1", "v1.2.3", "v2.0.0", "v9.999.999", "v10.0.0"]
        codes = [self.at_tag(tag)[1] for tag in ordered]
        self.assertEqual(codes, sorted(codes))
        self.assertEqual(len(codes), len(set(codes)), "two versions share a versionCode")

    def test_prerelease_and_build_metadata_keep_the_base_code(self):
        self.assertEqual(self.at_tag("v1.2.3-rc1"), ("1.2.3-rc1", 1_002_003))
        self.assertEqual(self.at_tag("v1.2.3+build7"), ("1.2.3+build7", 1_002_003))

    def test_four_components_are_rejected(self):
        """The regression this regex was anchored for.

        Unanchored, 'v1.2.3.4' matched its own prefix: versionName kept the .4
        while versionCode silently became 1002003 — so v1.2.3.4 and v1.2.3.5
        collided and Play refused the second upload for no visible reason.
        """
        with self.assertRaises(cfgmod.ConfigError):
            self.at_tag("v1.2.3.4")

    def test_malformed_tags_are_rejected_rather_than_guessed(self):
        for tag in ("v1.2", "v1", "vX.Y.Z", "v1.2.3-", "v 1.2.3", "v1.2.3 "):
            with self.subTest(tag=tag), self.assertRaises(cfgmod.ConfigError):
                self.at_tag(tag)

    def test_components_over_999_are_rejected(self):
        """Above 999 the packing stops being monotonic, so it is refused."""
        for tag in ("v1.1000.0", "v1.0.1000"):
            with self.subTest(tag=tag), self.assertRaises(cfgmod.ConfigError):
                self.at_tag(tag)

    def test_tag_without_v_falls_back_and_says_so(self):
        with quiet():
            self.assertEqual(self.at_tag("1.2.3"), ("0.0.0-dev", 1))


class ConfigureTargetsTest(unittest.TestCase):
    """expand_targets(): groups overlap on purpose, and must not double up."""

    def test_all_is_every_target(self):
        """Derived from TARGETS rather than a number written here. A count in a
        test goes stale the day a target is added, and it goes stale as
        "16 != 15", which says nothing about what is being asserted: that `all`
        holds everything, with nothing quietly left out."""
        result = cfgmod.expand_targets(["all"], [])
        self.assertEqual(sorted(result), sorted(cfgmod.TARGETS))

    def test_overlapping_groups_deduplicate(self):
        """desktop, linux and windows overlap heavily. The point is that nothing
        appears twice — not that the union equals any one of them, which stopped
        being true when linux gained a member desktop does not have."""
        result = cfgmod.expand_targets(["desktop", "linux", "windows"], [])
        self.assertEqual(len(result), len(set(result)))
        self.assertEqual(set(result),
                         set(cfgmod.GROUPS["desktop"]) | set(cfgmod.GROUPS["linux"])
                         | set(cfgmod.GROUPS["windows"]))

    def test_a_target_named_twice_appears_once(self):
        self.assertEqual(cfgmod.expand_targets(["web", "web", "web"], []), ["web"])

    def test_group_plus_one_of_its_own_members(self):
        self.assertEqual(cfgmod.expand_targets(["linux", "linux-x64-glibc"], []),
                         cfgmod.expand_targets(["linux"], []))

    def test_order_follows_the_declaration_order_not_the_input(self):
        """Stable output, whatever order the .toml lists things in."""
        self.assertEqual(cfgmod.expand_targets(["web", "linux-x64-glibc", "android"], []),
                         cfgmod.expand_targets(["android", "web", "linux-x64-glibc"], []))
        self.assertEqual(cfgmod.expand_targets(["web", "linux-x64-glibc"], []),
                         ["linux-x64-glibc", "web"])

    def test_disabled_subtracts_from_a_group(self):
        result = cfgmod.expand_targets(["all"], ["ios"])
        self.assertNotIn("ios", result)
        self.assertIn("macos", result)
        self.assertEqual(len(result), len(cfgmod.TARGETS) - 1)  # ios

    def test_disabled_can_be_a_group_too(self):
        result = cfgmod.expand_targets(["all"], ["bsd"])
        expected = len(cfgmod.TARGETS) - len(cfgmod.GROUPS["bsd"])
        self.assertEqual(len(result), expected)
        self.assertFalse([t for t in result if "bsd" in t])

    def test_disabling_something_not_enabled_is_harmless(self):
        self.assertEqual(cfgmod.expand_targets(["web"], ["android"]), ["web"])

    def test_drm_comes_with_all_now_and_can_still_be_switched_off(self):
        """It used to be held out of `all` because CI could not run it. It can
        now, so the only reason left was inertia. Turning it off is still one
        line, which is the part that has to keep working."""
        self.assertIn("linux-x64-glibc-drm", cfgmod.expand_targets(["all"], []))
        self.assertIn("linux-x64-glibc-drm", cfgmod.expand_targets(["drm"], []))
        self.assertNotIn("linux-x64-glibc-drm",
                         cfgmod.expand_targets(["all"], ["linux-x64-glibc-drm"]))
        self.assertNotIn("linux-x64-glibc-drm", cfgmod.expand_targets(["all"], ["drm"]))

    def test_unknown_names_are_rejected_in_both_lists(self):
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.expand_targets(["playstation"], [])
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.expand_targets(["all"], ["playstation"])

    def test_subtracting_everything_is_an_error_not_an_empty_build(self):
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.expand_targets(["all"], ["all"])
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.expand_targets([], [])


class ConfigureUpxTest(unittest.TestCase):
    """expand_upx(): the same group algebra as targets, and a shorter world.

    The refusals matter more than the expansions. Compressing a signed macOS
    binary breaks its signature and Gatekeeper refuses to launch it — that is
    not a preference, and a silent no-op would leave someone wondering why the
    setting did nothing.
    """

    def all_targets(self):
        return cfgmod.expand_targets(["all"], [])

    def upx(self, enabled, disabled=None, targets=None):
        # The whole section, not just the two lists: base_config() replaces the
        # dict wholesale, so anything left out here is missing rather than
        # defaulted, and validate() would fail on the hole instead of the test.
        cfg = base_config(upx={"enabled": enabled, "disabled": disabled or [],
                               "max_size_mb": cfgmod.DEFAULTS["upx"]["max_size_mb"]})
        return cfgmod.expand_upx(cfg, targets if targets is not None else self.all_targets())

    def test_the_default_is_linux_only(self):
        self.assertEqual(self.upx(["linux-x64-glibc", "linux-arm64-glibc"]), ["linux-x64-glibc", "linux-arm64-glibc"])

    def test_all_is_every_compressible_target_and_no_more(self):
        result = self.upx(["all"])
        for absent in ("macos", "ios", "android", "web"):
            self.assertNotIn(absent, result)
        self.assertIn("windows-x64", result)
        self.assertIn("netbsd-x64", result)

    def test_groups_overlap_without_doubling_up(self):
        result = self.upx(["linux", "all", "linux-x64-glibc"])
        self.assertEqual(len(result), len(set(result)))
        self.assertEqual(result, self.upx(["all"]))

    def test_disabled_subtracts_and_can_be_a_group(self):
        self.assertNotIn("freebsd-x64", self.upx(["all"], ["bsd"]))
        self.assertNotIn("windows-x64", self.upx(["all"], ["windows-x64"]))

    def test_order_is_stable_whatever_the_input_order(self):
        self.assertEqual(self.upx(["windows", "linux"]), self.upx(["linux", "windows"]))

    def test_the_signed_and_zipped_platforms_are_refused_with_a_reason(self):
        for refused, needle in (("macos", "signature"), ("ios", "signature"),
                                ("android", "APK"), ("web", "wasm")):
            with self.subTest(target=refused):
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    self.upx([refused])
                self.assertIn(needle, str(caught.exception))

    def test_unknown_names_are_rejected(self):
        with self.assertRaises(cfgmod.ConfigError):
            self.upx(["playstation"])

    def test_asking_for_a_target_you_are_not_building_is_a_no_op(self):
        """Not an error: `enabled = ["all"]` with a narrow [targets] should
        just compress the ones that exist, the same way disabling a target you
        never enabled is harmless."""
        self.assertEqual(self.upx(["all"], targets=["web", "linux-x64-glibc"]), ["linux-x64-glibc"])

    def test_empty_is_allowed_and_means_compress_nothing(self):
        self.assertEqual(self.upx([]), [])

    def test_the_lists_have_to_be_lists(self):
        cfg = base_config(upx__enabled="linux-x64-glibc")
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(cfg, False)

    def test_validate_refuses_what_cannot_be_compressed(self):
        """The refusals lived in expand_upx(), which only the packaging step
        calls (--print-upx), so `configure.py --check` passed
        `enabled = ["macos"]` and the release failed twenty minutes in."""
        for where in ("enabled", "disabled"):
            for refused, needle in (("macos", "signature"), ("ios", "signature"),
                                    ("android", "APK"), ("web", "wasm")):
                with self.subTest(where=where, target=refused):
                    cfg = base_config(**{f"upx__{where}": [refused]})
                    with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                        cfgmod.validate(cfg, False)
                    self.assertIn(f"[upx] {where}: {refused!r} cannot be compressed",
                                  str(caught.exception))
                    self.assertIn(needle, str(caught.exception))
                    self.assertIsNotNone(cfgmod.locate_from(caught.exception))

    def test_validate_refuses_an_unknown_name(self):
        for where in ("enabled", "disabled"):
            with self.subTest(where=where):
                cfg = base_config(**{f"upx__{where}": ["playstation"]})
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(cfg, False)
                self.assertIn(f"[upx] {where}: unknown target or group 'playstation'",
                              str(caught.exception))
                self.assertIsNotNone(cfgmod.locate_from(caught.exception))

    def test_each_group_is_every_compressible_target_of_its_family(self):
        """UPX_GROUPS["linux"] was a list somebody kept, and it left out
        linux-arm64-glibc-drm: `enabled = ["linux"]` did not compress it."""
        for group in ("linux", "windows", "bsd"):
            with self.subTest(group=group):
                family = [t for t in cfgmod.UPX_TARGETS if cfgmod.TARGETS[t][0] == group]
                self.assertEqual(sorted(cfgmod.UPX_GROUPS[group]), sorted(family))
        self.assertEqual(sorted(cfgmod.UPX_GROUPS["all"]), sorted(cfgmod.UPX_TARGETS))
        every = set(cfgmod.TARGETS)
        self.assertEqual(set(cfgmod.UPX_TARGETS) | set(cfgmod.UPX_REFUSED), every,
                         "every target is either compressible or refused with a reason")


class ConfigureUpxSizeCapTest(unittest.TestCase):
    """[upx] max_size_mb: the ceiling above which the binary is left alone.

    UPX cannot pack a file past roughly 768 MB and exits non-zero when asked,
    which would fail a release rather than decline it. The cap exists so the
    packer skips instead, and every rejection below is a value that would have
    turned that skip back into a build failure.
    """

    def cap(self, value):
        cfg = base_config(upx__max_size_mb=value)
        with quiet():
            cfgmod.validate(cfg, False)

    def test_the_default_leaves_room_under_the_hard_limit(self):
        default = cfgmod.DEFAULTS["upx"]["max_size_mb"]
        self.assertEqual(default, 600)
        self.assertLess(default, cfgmod.UPX_HARD_LIMIT_MB)

    def test_a_plain_number_is_accepted(self):
        self.cap(1)
        self.cap(600)

    def test_the_hard_limit_itself_is_the_last_accepted_value(self):
        self.cap(cfgmod.UPX_HARD_LIMIT_MB)
        with self.assertRaises(cfgmod.ConfigError):
            self.cap(cfgmod.UPX_HARD_LIMIT_MB + 1)

    def test_above_the_hard_limit_is_rejected_and_says_why(self):
        with self.assertRaises(cfgmod.ConfigError) as caught:
            self.cap(2000)
        self.assertIn(str(cfgmod.UPX_HARD_LIMIT_MB), str(caught.exception))

    def test_zero_and_negative_are_rejected_towards_the_right_setting(self):
        for value in (0, -1):
            with self.subTest(value=value):
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    self.cap(value)
                self.assertIn("enabled", str(caught.exception))

    def test_a_bool_is_not_a_number(self):
        """`max_size_mb = true` in the TOML. Python says isinstance(True, int),
        so without an explicit check this would be accepted as 1 MB and quietly
        skip every binary in the project."""
        for value in (True, False):
            with self.subTest(value=value):
                with self.assertRaises(cfgmod.ConfigError):
                    self.cap(value)

    def test_strings_and_floats_are_rejected(self):
        for value in ("600", "600mb", 600.5, None, [600]):
            with self.subTest(value=value):
                with self.assertRaises(cfgmod.ConfigError):
                    self.cap(value)

    def test_a_toml_that_omits_it_gets_the_default(self):
        """The whole point of DEFAULTS: an existing config written before this
        setting existed keeps working, with the margin already applied."""
        merged = cfgmod.deep_merge(cfgmod.DEFAULTS, {"upx": {"enabled": ["linux-x64-glibc"]}})
        self.assertEqual(merged["upx"]["max_size_mb"],
                         cfgmod.DEFAULTS["upx"]["max_size_mb"])


class ConfigureValidateTest(unittest.TestCase):
    """validate(): every rejection, because each one is a build it saved."""

    def assert_rejects(self, cfg, needle=None, strict=False):
        with self.assertRaises(cfgmod.ConfigError) as caught:
            with quiet():
                cfgmod.validate(cfg, strict)
        if needle:
            self.assertIn(needle, str(caught.exception))

    def test_the_defaults_are_valid(self):
        with quiet():
            cfgmod.validate(base_config(), False)

    def test_project_name_becomes_a_filename(self):
        for bad in ("my game", "", "juego/1", "a.b", "1game"):
            with self.subTest(name=bad):
                self.assert_rejects(base_config(project={"name": bad}))

    def test_window_title_must_be_one_line(self):
        cfg = base_config()
        cfg["window"]["title"] = "two\nlines"
        self.assert_rejects(cfg, "single line")

    def test_window_size_bounds(self):
        for value in (0, -1, 15, 20000, "800", 1.5):
            with self.subTest(value=value):
                cfg = base_config()
                cfg["window"]["width"] = value
                self.assert_rejects(cfg)

    def test_orientation(self):
        cfg = base_config()
        cfg["window"]["orientation"] = "sideways"
        self.assert_rejects(cfg)

    def test_example_identifiers_are_refused_on_a_release(self):
        """The one that is irreversible on Google Play once published."""
        cfg = copy.deepcopy(cfgmod.DEFAULTS)   # still com.example.*
        with quiet():
            cfgmod.validate(cfg, False)        # fine while developing
        self.assert_rejects(cfg, strict=True)  # never on a release

    def test_application_id_shape(self):
        for bad in ("nodots", "com..game", "1com.game", "com.game-over", ""):
            with self.subTest(appid=bad):
                self.assert_rejects(base_config(android__application_id=bad))

    def test_application_id_allows_uppercase(self):
        """Android's rule is alphanumerics and underscores per segment, each
        starting with a letter. Uppercase is legal even though the Java package
        convention is lowercase — rejecting it would be us inventing a rule."""
        with quiet():
            cfgmod.validate(base_config(android__application_id="com.Example.Game"), False)

    def test_gl_version_and_category(self):
        self.assert_rejects(base_config(android__gl_version="ES10"))
        self.assert_rejects(base_config(android__category="videogame"))

    def test_boolean_tables_reject_non_booleans(self):
        cfg = base_config()
        cfg["android"]["display"]["keep_on"] = "yes"
        self.assert_rejects(cfg, "true or false")

    def test_ios_deployment_target(self):
        self.assert_rejects(base_config(ios__deployment_target="fifteen"))

    def test_ios_below_13_is_refused_because_of_std_filesystem(self):
        for too_old in ("12.4", "11.0", "9.3", "12.99.1"):
            with self.subTest(target=too_old):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(base_config(ios__deployment_target=too_old), False)
                self.assertIn("below 13.0", str(caught.exception))
                self.assertIn("std::filesystem", str(caught.exception))
        for fine in ("13.0", "15.6", "18.2.1"):
            with self.subTest(target=fine), quiet():
                cfgmod.validate(base_config(ios__deployment_target=fine), False)

    def test_raylib_modules_must_exist_and_be_optional(self):
        self.assert_rejects(base_config(raylib={"disabled_modules": ["rcore"]}))
        self.assert_rejects(base_config(raylib={"disabled_modules": ["rnothing"]}))

    def test_web_memory_bounds(self):
        for value in (4, 8192, "64", 64.5):
            with self.subTest(value=value):
                self.assert_rejects(base_config(web={"memory": value, "grow": False}))

    def test_linux_backend_and_wayland(self):
        self.assert_rejects(base_config(linux={"backend": "sdl", "wayland": False}))
        cfg = base_config()
        cfg["linux"]["wayland"] = "yes"
        self.assert_rejects(cfg, "true or false")

    def test_rgfw_and_wayland_cannot_both_hold(self):
        """Individually valid, together a contradiction — and the build would
        silently pick X11 while the config said Wayland. Found by trying it:
        RGFW reports itself as "DESKTOP (RGFW - X11)"."""
        self.assert_rejects(base_config(linux={"backend": "rgfw", "wayland": True}),
                            "cannot both hold")

    def test_rgfw_without_wayland_is_fine(self):
        with quiet():
            cfgmod.validate(base_config(linux={"backend": "rgfw", "wayland": False}), False)

    def test_glfw_with_wayland_is_fine(self):
        with quiet():
            cfgmod.validate(base_config(linux={"backend": "glfw", "wayland": True}), False)

    def test_compiler_choices(self):
        # Pinned to Windows, because mingw and msvc are only meaningful there —
        # see ConfigureHostToolchainTest for the rule that makes that true.
        # This test used to pass on any host, which stopped being correct the
        # moment the host check existed, and it is the test that said so.
        original = cfgmod.platform.system
        cfgmod.platform.system = lambda: "Windows"
        try:
            for good in ("clang", "gcc", "mingw", "msvc", "default"):
                with self.subTest(compiler=good), quiet():
                    cfgmod.validate(base_config(dev={"compiler": good, "linker": "auto"}),
                                    False)
            self.assert_rejects(base_config(dev={"compiler": "icc", "linker": "auto"}))
        finally:
            cfgmod.platform.system = original

    def test_dev_strict_is_a_bool_and_reaches_the_header(self):
        """[dev] strict: the two values it can take, every type it cannot, and
        the generated macro that carries it -- because a setting that validates
        and then never reaches the compiler is a setting that does nothing."""
        for good in (True, False):
            with self.subTest(strict=good), quiet():
                cfg = base_config(dev={"compiler": "clang", "linker": "auto",
                                       "strict": good})
                cfgmod.validate(cfg, False)
        for bad in ("true", "yes", 1, 0, 1.0, None, [True], {"on": True}):
            with self.subTest(strict=bad):
                self.assert_rejects(
                    base_config(dev={"compiler": "clang", "linker": "auto",
                                     "strict": bad}),
                    "true or false")
        with generated_header(base_config(dev={"compiler": "clang", "linker": "auto",
                                               "strict": True})) as header:
            self.assertIn("#define RMP_DEV_STRICT      1", header)
        with generated_header(base_config()) as header:
            self.assertIn("#define RMP_DEV_STRICT      0", header)
        # And the entry point reads it; a macro nobody tests is a comment.
        app_cpp = (REPO / "src" / "rmp" / "app.cpp").read_text()
        self.assertIn("RMP_DEV_STRICT", app_cpp)
        self.assertIn("!defined(NDEBUG)", app_cpp)

    def test_windows_backend(self):
        self.assert_rejects(base_config(windows={"backend": "sdl"}))


class GeneratedFallbacksTest(unittest.TestCase):
    """A source file that guards a generated value with `#ifndef RMP_X` names
    a value the generator really emits.

    The fallbacks (`#ifndef RMP_UI_FONT_SIZE / #define RMP_UI_FONT_SIZE 20`)
    exist so a header compiles before configure.py has run. If the generator
    renames a value and a fallback does not follow, nothing fails: the
    fallback quietly becomes the only definition, and the .toml setting stops
    reaching that file. That is exactly what a rename of every generated name
    could have left behind, so every guard is checked against the header the
    generator writes from the defaults."""

    def test_every_fallback_guards_a_generated_name(self):
        with generated_header(base_config()) as header:
            emitted = set(re.findall(r"^#define\s+(RMP_\w+)", header, re.M))
        self.assertIn("RMP_WINDOW_WIDTH", emitted)
        guarded = {}
        for root in ("include", "src", "tests", "examples"):
            for path in sorted((REPO / root).rglob("*")):
                if path.suffix not in (".h", ".c", ".cpp") or "generated" in path.parts:
                    continue
                for name in re.findall(r"^\s*#\s*ifndef\s+(RMP_\w+)",
                                       path.read_text(errors="replace"), re.M):
                    guarded.setdefault(name, path.relative_to(REPO).as_posix())
        # The include guard of the smoke-test header is a guard, not a fallback.
        guarded.pop("RMP_SMOKE_TEST_H", None)
        self.assertGreaterEqual(len(guarded), 5, "the fallbacks were not found at all")
        for name, where in sorted(guarded.items()):
            with self.subTest(name=name):
                self.assertIn(name, emitted,
                              f"{where} falls back on {name}, which configure.py "
                              f"does not generate")


class ProductionBuildEverywhereTest(unittest.TestCase):
    """RMP_PRODUCTION_BUILD and RMP_RESOURCES_PATH are defined by every build
    system, and none of them still defines the names they replaced.

    The C macro PRODUCTION_BUILD existed on the desktop only: Android and iOS
    never defined it, while TECHNICAL.md told games to write
    `#if PRODUCTION_BUILD` -- which there is `#if 0`, silently. And Gradle
    never passed the CMake option at all, so raymob's CMake took its debug
    branch for the release variant too: every release APK and AAB was compiled
    at -O0 with _DEBUG. The iOS side is checked on the generated project, in
    ConfigureGeneratorsTest."""

    RAYMOB = REPO / "raymob" / "app" / "src" / "main" / "cpp" / "CMakeLists.txt"

    def test_the_desktop_build_defines_both(self):
        text = (REPO / "CMakeLists.txt").read_text()
        for line in ("target_compile_definitions(rmp PUBLIC RMP_PRODUCTION_BUILD=1)",
                     "target_compile_definitions(rmp PUBLIC RMP_PRODUCTION_BUILD=0)",
                     'RMP_RESOURCES_PATH="${RESOURCES_PATH}"'):
            self.assertTrue(line in text, f"CMakeLists.txt lacks {line}")

    def test_android_defines_both_and_takes_release_from_the_build_type(self):
        text = self.RAYMOB.read_text()
        self.assertTrue('RMP_RESOURCES_PATH="${RESOURCES_PATH}"' in text)
        self.assertTrue("PRIVATE RMP_PRODUCTION_BUILD=1)" in text)
        self.assertTrue("PRIVATE RMP_PRODUCTION_BUILD=0 _DEBUG DEBUG)" in text)
        # The option Gradle never set is gone; the build type decides.
        self.assertNotRegex(text, r"option\(\s*PRODUCTION_BUILD")
        self.assertRegex(text, r'CMAKE_BUILD_TYPE STREQUAL "Debug"')

    def test_the_msvc_syntax_pass_defines_both(self):
        text = (REPO / ".github" / "workflows" / "_windows.yml").read_text()
        for flag in ("/DRMP_PRODUCTION_BUILD=0", "/DRMP_RESOURCES_PATH="):
            self.assertTrue(flag in text, f"_windows.yml does not pass {flag}")

    def test_the_entry_point_refuses_a_build_system_that_forgot(self):
        text = (REPO / "src" / "rmp" / "app.cpp").read_text()
        self.assertRegex(text, r"#if !defined\(RMP_PRODUCTION_BUILD\)\s*\n#error")

    def test_no_build_system_defines_an_old_name(self):
        old = re.compile(r"(?:[-/]D|\s|\()(PRODUCTION_BUILD|RESOURCES_PATH|RRES_PASSWORD)=")
        files = [REPO / "CMakeLists.txt", self.RAYMOB, REPO / "tools" / "configure.py",
                 REPO / "tools" / "header_check.sh", REPO / "tools" / "header_cost.py",
                 *sorted((REPO / ".github" / "workflows").glob("*.yml"))]
        for path in files:
            for lineno, line in enumerate(path.read_text().splitlines(), 1):
                if line.lstrip().startswith("#"):
                    continue
                # -DPRODUCTION_BUILD=ON is the CMake OPTION, which keeps its
                # name; the macro took the RMP_ prefix.
                for m in old.finditer(line):
                    if m.group(1) == "PRODUCTION_BUILD" and re.search(
                            r"-DPRODUCTION_BUILD=(ON|OFF)\b", line):
                        continue
                    where = path.relative_to(REPO).as_posix()
                    with self.subTest(file=where, line=lineno):
                        self.fail(f"{where}:{lineno} defines {m.group(1)}: {line.strip()}")



class ReleaseHardeningTest(unittest.TestCase):
    """A Linux or BSD release is compiled and linked with the hardening in
    CMakeLists.txt (RMP_RELEASE_HARDENING), read back out of a release tree
    configured here: the compile commands of the framework, the game and
    raylib, and the game's link line. What the linker made of it is read off
    the shipped binary by `binary_check.py elf` in the Linux jobs."""

    @classmethod
    def setUpClass(cls):
        import shutil
        import subprocess
        cls.skip = None
        if not sys.platform.startswith("linux"):
            cls.skip = "the flags are read on Linux, where CI builds the release"
            return
        if shutil.which("cmake") is None or shutil.which("ninja") is None:
            if IN_BUILD_IMAGE:
                raise AssertionError("the build image has cmake and ninja; this cannot skip there")
            cls.skip = "cmake or ninja is not installed"
            return
        cls._tmp = tempfile.TemporaryDirectory()
        # A copy of the checkout, configured there: CMake runs configure.py,
        # and no test here may write the checkout's generated files (see
        # GENERATED at the top). Without what is generated or built.
        source = Path(cls._tmp.name) / "src"
        shutil.copytree(REPO, source, symlinks=True, ignore=shutil.ignore_patterns(
            ".git", "build", ".zig-*", "generated", "project.yml"))
        cls.tree = Path(cls._tmp.name) / "build"
        got = subprocess.run(["cmake", "-S", str(source), "-B", str(cls.tree), "-G", "Ninja",
                              "-DCMAKE_BUILD_TYPE=Release", "-DPRODUCTION_BUILD=ON"],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        cls.configure = got.stdout + got.stderr
        if got.returncode != 0:
            raise AssertionError("the release tree did not configure:\n" + cls.configure[-3000:])
        cls.commands = json.loads((cls.tree / "compile_commands.json").read_text())
        cls.ninja = (cls.tree / "build.ninja").read_text()

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "_tmp", None) is not None:
            cls._tmp.cleanup()

    def setUp(self):
        if self.skip:
            self.skipTest(self.skip)

    def command_for(self, suffix: str) -> str:
        found = [e["command"] for e in self.commands if e["file"].endswith(suffix)]
        self.assertTrue(found, f"no compile command for {suffix}")
        return found[0]

    def link_line(self, target: str) -> str:
        at = self.ninja.index(f"build {target}: CXX_EXECUTABLE_LINKER")
        block = self.ninja[at:self.ninja.index("\n\n", at)]
        return block

    def libc_is_glibc(self) -> bool:
        import subprocess
        got = subprocess.run(["sh", "-c", "printf '#include <features.h>\\n__GLIBC__\\n' | "
                                          "cc -E -x c - 2>/dev/null | tail -1"],
                             capture_output=True, text=True)
        return got.stdout.strip().isdigit()

    def test_every_part_of_the_binary_is_compiled_hardened(self):
        for suffix in ("src/rmp/app.cpp", "src/rmp/scene.cpp", "src/main.cpp",
                       "thirdparty/raylib/src/rcore.c"):
            with self.subTest(file=suffix):
                command = self.command_for(suffix)
                self.assertIn("-fstack-protector-strong", command)
                self.assertIn("-fstack-clash-protection", command)
                if self.libc_is_glibc():
                    self.assertRegex(command, r"-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=[23]\b")

    def test_the_cxx_library_checks_its_bounds(self):
        command = self.command_for("src/rmp/app.cpp")
        self.assertIn("-D_GLIBCXX_ASSERTIONS", command)
        self.assertIn("-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST", command)

    def test_the_game_is_linked_hardened(self):
        name = cfgmod.load_config()["project"]["name"]
        line = self.link_line(name)
        for flag in ("-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack"):
            with self.subTest(flag=flag):
                self.assertIn(flag, line)

    def test_pie_is_on_and_says_what_it_costs(self):
        """+5.5% on linux-x64-glibc, over the 3% a release flag may cost, and on
        all the same: Omar's decision of 2026-10-10, written next to the
        number. The game is linked -pie and every object it is made of is
        position-independent, or the link would refuse them: -fPIE for the
        game's own files, -fPIC for the libraries it links (rmp, raylib),
        which is what CMake gives a static library and is as good."""
        cmake = (REPO / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'option\(RMP_RELEASE_PIE "[^"]*\+5\.5%[^"]*" ON\)')
        self.assertIn("ADOPTED over the 3% rule, by\n#       Omar's decision", cmake)
        self.assertIn("-pie", self.link_line(cfgmod.load_config()["project"]["name"]).split())
        for unit, flag in (("src/main.cpp", "-fPIE"), ("src/rmp/app.cpp", "-fPIE"),
                           ("src/rmp/scene.cpp", "-fPIC"),
                           ("thirdparty/raylib/src/rcore.c", "-fPIC")):
            with self.subTest(unit=unit):
                self.assertIn(flag, self.command_for(unit).split())

    def test_the_configure_says_what_it_applied(self):
        self.assertIn("=== RELEASE HARDENING:", self.configure)



class LinuxJobsReadTheHardeningTest(unittest.TestCase):
    """Every Linux release job reads the hardening off the binary it ships,
    with tools/binary_check.py elf, before UPX packs it (a packed ELF has
    UPX's headers, not the linker's)."""

    JOBS = ("x64", "arm64", "musl-x64", "riscv64", "drm-x64", "drm-arm64")

    def test_each_job_checks_its_binary_before_packing_it(self):
        linux = REPO / ".github" / "workflows" / "_linux.yml"
        for job in self.JOBS:
            with self.subTest(job=job):
                text = job_block(linux, job)
                self.assertIn("python3 tools/binary_check.py elf", text)
                self.assertIn("RMP_RELEASE_PIE:BOOL=ON", text)
                if "upx_pack.sh" in text:
                    self.assertLess(text.index("binary_check.py elf"), text.index("upx_pack.sh"))

    def run_step(self, job, cache):
        """The job's `Linked hardened` step, run in a folder holding `cache` as
        build/release/CMakeCache.txt (None: no cache), with a python3 that
        writes down what it was asked."""
        import subprocess
        import textwrap
        step = step_block(job_block(REPO / ".github" / "workflows" / "_linux.yml", job),
                          "Linked hardened")
        self.assertIn("        run: |\n", step)
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        script = script.replace("${{ inputs.project_name }}", "demo")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "bin").mkdir()
            stub = root / "bin" / "python3"
            stub.write_text('#!/bin/sh\necho "$*" > "$(dirname "$0")/asked"\n')
            stub.chmod(0o755)
            if cache is not None:
                (root / "build" / "release").mkdir(parents=True)
                (root / "build" / "release" / "CMakeCache.txt").write_text(cache)
            got = subprocess.run(["bash", "-c", script], cwd=root, capture_output=True,
                                 text=True, env=dict(os.environ, PATH=f"{root / 'bin'}:"
                                                     f"{os.environ['PATH']}"))
            asked = (root / "bin" / "asked").read_text() if (root / "bin" / "asked").exists() \
                else None
        return got.returncode, asked, got.stdout + got.stderr

    def test_the_step_reads_the_release_cache_and_a_missing_one_fails_it(self):
        """`if grep -q ... build/CMakeCache.txt` read a cache that was not
        there as "PIE off", and the check ran without --pie and passed: what
        the step did the day the release moved to build/release. Seen red on
        that step -- exit 0 with no cache, and no --pie with one."""
        if sys.platform == "win32":
            self.skipTest("the step is bash")
        for job in self.JOBS:
            with self.subTest(job=job):
                code, asked, out = self.run_step(job, None)
                self.assertNotEqual(code, 0, out)
                self.assertIsNone(asked, "the check ran without the cache")
                self.assertIn("says nothing of RMP_RELEASE_PIE", out)
                code, asked, out = self.run_step(job, "RMP_RELEASE_PIE:BOOL=ON\n")
                self.assertEqual(code, 0, out)
                self.assertEqual(asked.split(), ["tools/binary_check.py", "elf", "--pie",
                                                 "build/release/demo"])
                code, asked, out = self.run_step(job, "RMP_RELEASE_PIE:BOOL=OFF\n")
                self.assertEqual(code, 0, out)
                self.assertEqual(asked.split(), ["tools/binary_check.py", "elf",
                                                 "build/release/demo"])

class AndroidReleaseCheckTest(unittest.TestCase):
    """tools/android_release_check.py, the Android job's proof that the release
    variant was compiled as a release, seen red on the database the old
    raymob CMakeLists.txt produced."""

    SCRIPT = REPO / "tools" / "android_release_check.py"

    def run_on(self, variant, *commands):
        import subprocess, tempfile, json as _json
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / variant / "4x5y6z" / "arm64-v8a"
            db.mkdir(parents=True)
            entries = [{"directory": "/b",
                        "file": name if name.startswith("/") else f"/w/src/{name}",
                        "command": cmd} for name, cmd in commands]
            (db / "compile_commands.json").write_text(_json.dumps(entries))
            return subprocess.run(["python3", str(self.SCRIPT), tmp],
                                  capture_output=True, text=True)

    RELEASE = "clang++ -DRMP_PRODUCTION_BUILD=1 -O2 -g -DNDEBUG -O3 -flto -c x.cpp"
    OLD = "clang++ -D_DEBUG -DDEBUG -O2 -g -DNDEBUG -O0 -c x.cpp"

    def test_a_release_compiled_as_a_release_passes(self):
        got = self.run_on("RelWithDebInfo", ("rmp/app.cpp", self.RELEASE),
                          ("scenes/main_menu.cpp", self.RELEASE))
        self.assertEqual(got.returncode, 0, got.stdout)
        self.assertIn("PASS: 2 release", got.stdout)

    def test_what_the_old_cmake_produced_fails(self):
        got = self.run_on("RelWithDebInfo", ("rmp/app.cpp", self.OLD))
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("-O0", got.stdout)
        self.assertIn("no -DRMP_PRODUCTION_BUILD=1", got.stdout)
        self.assertIn("_DEBUG", got.stdout)

    def test_raylib_is_checked_for_its_optimisation_only(self):
        """raylib's sources sit under thirdparty/raylib/src/ and never get our
        define; the old branch forced -O0 on them too, and that is what fails."""
        ok = self.run_on("RelWithDebInfo", ("rmp/app.cpp", self.RELEASE),
                         ("/w/thirdparty/raylib/src/rcore.c", "clang -O2 -O3 -c rcore.c"))
        self.assertEqual(ok.returncode, 0, ok.stdout)
        bad = self.run_on("RelWithDebInfo", ("rmp/app.cpp", self.RELEASE),
                          ("/w/thirdparty/raylib/src/rcore.c", "clang -O2 -O0 -c rcore.c"))
        self.assertEqual(bad.returncode, 1, bad.stdout)
        self.assertIn("rcore.c: -O0", bad.stdout)

    def test_no_release_database_fails(self):
        got = self.run_on("Debug", ("rmp/app.cpp", self.OLD))
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("no release compile database", got.stdout)

    def test_a_database_with_nothing_from_src_fails(self):
        import subprocess, tempfile, json as _json
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "RelWithDebInfo" / "h" / "x86_64"
            db.mkdir(parents=True)
            (db / "compile_commands.json").write_text(_json.dumps(
                [{"directory": "/b", "file": "/w/thirdparty/raylib/src2/rcore.c",
                  "command": self.RELEASE}]))
            got = subprocess.run(["python3", str(self.SCRIPT), tmp],
                                 capture_output=True, text=True)
        self.assertEqual(got.returncode, 1, got.stdout)

    def test_it_runs_after_the_release_build(self):
        text = (REPO / ".github" / "workflows" / "_android.yml").read_text()
        build = text.index("./gradlew bundleRelease")
        check = text.index("tools/android_release_check.py raymob/app/.cxx")
        self.assertGreater(check, build)


class ConfigureHelpersTest(unittest.TestCase):
    """The small pure functions whose output ends up inside a generated file."""

    def test_deep_merge_overrides_nested_without_touching_the_base(self):
        base = {"a": {"x": 1, "y": 2}, "b": 3}
        frozen = copy.deepcopy(base)
        merged = cfgmod.deep_merge(base, {"a": {"y": 99}})
        self.assertEqual(merged, {"a": {"x": 1, "y": 99}, "b": 3})
        self.assertEqual(base, frozen, "deep_merge must not mutate its input")

    def test_cmake_escape(self):
        self.assertEqual(cfgmod.cmake_escape('say "hi"'), 'say \\"hi\\"')
        self.assertEqual(cfgmod.cmake_escape(r"C:\games"), r"C:\\games")

    def test_yaml_scalar_quotes_what_would_break_the_xcode_spec(self):
        self.assertEqual(cfgmod.yaml_scalar(True), "YES")
        self.assertEqual(cfgmod.yaml_scalar(False), "NO")
        self.assertEqual(cfgmod.yaml_scalar(15), "15")
        self.assertEqual(cfgmod.yaml_scalar("plain"), "plain")
        for needs_quotes in ("", "a: b", "#hash", "[list]", "with, comma", " padded "):
            with self.subTest(value=needs_quotes):
                self.assertTrue(cfgmod.yaml_scalar(needs_quotes).startswith('"'))
        self.assertEqual(cfgmod.yaml_scalar('say "hi"'), '"say \\"hi\\""')

    def test_admob_needs_both_the_switch_and_an_android_build(self):
        cfg = base_config()
        cfg["android"]["admob"]["enabled"] = True
        self.assertTrue(cfgmod.admob_on(cfg, ["android", "web"]))
        self.assertFalse(cfgmod.admob_on(cfg, ["web"]),
                         "no Android build means no ads, whatever the switch says")
        cfg["android"]["admob"]["enabled"] = False
        self.assertFalse(cfgmod.admob_on(cfg, ["android"]))

    def test_admob_is_off_unless_a_game_turns_it_on(self):
        """The README says ads are opt-in, and they were on: DEFAULTS and the
        shipped .toml both said enabled = true, so every game made with
        `rmp new` carried the Google Mobile Ads SDK, AD_ID and INTERNET and
        owed Play a Data safety declaration for an ad it never showed."""
        self.assertIs(cfgmod.DEFAULTS["android"]["admob"]["enabled"], False)
        with quiet():
            self.assertFalse(cfgmod.admob_on(base_config(), ["android"]))

    def test_the_framework_keeps_it_on_so_its_android_job_builds_the_ads_path(self):
        text = (REPO / "raylib_multiplatform.toml").read_text()
        section = text[text.index("\n[android.admob]"):]
        section = section[:section.index("\n[", 1)]
        self.assertRegex(section, r"\nenabled = true\n")
        # And the comment above it says what turning it on brings.
        for brings in ("Google Mobile Ads", "AD_ID", "INTERNET", "Data safety"):
            with self.subTest(brings=brings):
                self.assertIn(brings, section)


class ConfigureRaylibFlagsTest(unittest.TestCase):
    """raylib_defs(): the complete set, or a feature silently turns itself off."""

    def test_external_config_flags_is_always_first(self):
        defs, _ = cfgmod.raylib_defs(base_config())
        self.assertEqual(defs[0], "EXTERNAL_CONFIG_FLAGS")

    def test_every_module_is_on_by_default(self):
        defs, _ = cfgmod.raylib_defs(base_config())
        for flag in cfgmod.MODULE_FLAG.values():
            self.assertIn(f"{flag}=1", defs)

    def test_disabling_a_module_sets_it_to_zero_and_leaves_the_rest_alone(self):
        defs, _ = cfgmod.raylib_defs(base_config(raylib={"disabled_modules": ["raudio"]}))
        self.assertIn("SUPPORT_MODULE_RAUDIO=0", defs)
        self.assertIn("SUPPORT_MODULE_RTEXTURES=1", defs)

    def test_the_whole_set_is_carried_over(self):
        """Defining EXTERNAL_CONFIG_FLAGS skips config.h's defaults entirely, so
        a flag we drop becomes undefined — and `#if SUPPORT_X` reads undefined
        as 0, turning off something nobody asked to turn off."""
        parsed = cfgmod.parse_raylib_config()
        defs, _ = cfgmod.raylib_defs(base_config())
        self.assertEqual(len(defs), len(parsed) + 1)
        self.assertGreater(len(parsed), 40, "config.h should define far more than a handful")


class ConfigureGeneratorsTest(unittest.TestCase):
    """The generated files, parsed by something other than the generator.

    These three are the ones whose mistakes only surface on ONE platform,
    twenty minutes into a matrix run: a malformed manifest fails the Android
    job, a broken YAML spec fails the iOS job, and neither says anything useful
    on a Linux machine. Parsing the output with a real parser catches them here.

    They write into a temp directory, not the repo: a test that leaves files
    behind is a test people learn to distrust.
    """

    def setUp(self):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        self._real_repo = cfgmod.REPO
        cfgmod.REPO = Path(self._tmp.name)
        # MANIFEST_OUT is computed from REPO at import time, so it has to be
        # moved too. MANIFEST_TEMPLATE is deliberately left pointing at the real
        # one: it is an input, and reading it is what makes this a real test.
        self._real_manifest_out = cfgmod.MANIFEST_OUT
        cfgmod.MANIFEST_OUT = Path(self._tmp.name) / "raymob" / "app" / "generated" / "AndroidManifest.xml"

    def tearDown(self):
        cfgmod.REPO = self._real_repo
        cfgmod.MANIFEST_OUT = self._real_manifest_out
        self._tmp.cleanup()

    def test_the_android_manifest_is_well_formed_xml(self):
        import xml.etree.ElementTree as ET
        cfg = base_config()
        with quiet():
            cfgmod.gen_android_manifest(cfg, ["android"])
        path = Path(self._tmp.name) / "raymob" / "app" / "generated" / "AndroidManifest.xml"
        self.assertTrue(path.is_file(), "the manifest was not written where Gradle looks")
        root = ET.parse(path).getroot()   # raises on malformed XML
        self.assertEqual(root.tag, "manifest")

    def test_permissions_are_elements_that_appear_and_disappear(self):
        """The caveat this generator exists for: android:required is only valid
        on <uses-feature> and is silently ignored on <uses-permission>, so a
        permission has to be ADDED or NOT, never declared optional."""
        import xml.etree.ElementTree as ET
        path = Path(self._tmp.name) / "raymob" / "app" / "generated" / "AndroidManifest.xml"

        cfg = base_config()
        cfg["android"]["permissions"]["vibration"] = False
        with quiet():
            cfgmod.gen_android_manifest(cfg, ["android"])
        self.assertNotIn("android.permission.VIBRATE", path.read_text())

        cfg["android"]["permissions"]["vibration"] = True
        with quiet():
            cfgmod.gen_android_manifest(cfg, ["android"])
        self.assertIn("android.permission.VIBRATE", path.read_text())
        ET.parse(path)

    def test_the_xcode_spec_is_well_formed_yaml(self):
        yaml = require_yaml(self)
        with quiet():
            cfgmod.gen_ios_project(base_config())
        path = Path(self._tmp.name) / "ios" / "project.yml"
        self.assertTrue(path.is_file())
        spec = yaml.safe_load(path.read_text())   # raises on malformed YAML
        self.assertIn("targets", spec)

    def test_a_title_full_of_yaml_metacharacters_survives_the_round_trip(self):
        """The reason yaml_scalar() exists, checked end to end rather than in
        isolation: a game called `Hero: "the game", v2 #1` must not break the
        spec that Xcode is generated from."""
        yaml = require_yaml(self)
        nasty = 'Hero: "the game", v2 #1'
        cfg = base_config()
        cfg["window"]["title"] = nasty
        with quiet():
            cfgmod.gen_ios_project(cfg)
        spec = yaml.safe_load((Path(self._tmp.name) / "ios" / "project.yml").read_text())
        self.assertIn(nasty, yaml.dump(spec))

    def test_the_ios_app_declares_a_launch_screen(self):
        """An iOS app without one runs in compatibility mode: a legacy
        resolution, letterboxed, so every scale the game derives from the
        viewport is derived from the wrong viewport -- and App Review has
        rejected submissions without a launch screen since April 2020.

        `UILaunchStoryboardName: ""` is not "no opinion", it is an app stating
        it has none, which is the case that gets rejected. Xcode 14+ generates
        a plain launch screen from UILaunchScreen_Generation, so there is no
        storyboard file to ship and nothing for the generator to keep in sync.
        """
        with quiet():
            cfgmod.gen_ios_project(base_config())
        text = (Path(self._tmp.name) / "ios" / "project.yml").read_text()

        self.assertIn("INFOPLIST_KEY_UILaunchScreen_Generation: YES", text)
        self.assertNotRegex(text, r'INFOPLIST_KEY_UILaunchStoryboardName:\s*""',
                            "an empty launch storyboard name is an app declaring it "
                            "has no launch screen, which is the App Store rejection")

        try:
            import yaml
        except ImportError:
            if IN_BUILD_IMAGE:
                self.fail("PyYAML is missing inside the build image; add python3-yaml")
            return   # The raw assertions above are the gate; the parse is a bonus
        spec = yaml.safe_load(text)
        settings = spec["targets"][base_config()["project"]["name"]]["settings"]["base"]
        self.assertTrue(settings["INFOPLIST_KEY_UILaunchScreen_Generation"])
        self.assertNotIn("INFOPLIST_KEY_UILaunchStoryboardName", settings)

    def test_the_ios_build_says_whether_it_is_a_release(self):
        """RMP_PRODUCTION_BUILD is 0 in Debug and 1 in Release, on iOS as on
        every other platform, and the list of definitions is whole in both:
        under XcodeGen's `configs` a key REPLACES the `base` one. And the
        [ios.settings] passthrough still lands in `base`, where it applies to
        both configurations, not inside the last one."""
        yaml = require_yaml(self)
        cfg = base_config()
        cfg["ios"]["settings"] = {"DEVELOPMENT_TEAM": "ABCDE12345"}
        with quiet():
            cfgmod.gen_ios_project(cfg)
        spec = yaml.safe_load((Path(self._tmp.name) / "ios" / "project.yml").read_text())
        settings = spec["targets"][cfg["project"]["name"]]["settings"]
        self.assertEqual(settings["base"]["DEVELOPMENT_TEAM"], "ABCDE12345")
        self.assertNotIn("GCC_PREPROCESSOR_DEFINITIONS", settings["base"])
        for config, value in (("Debug", "0"), ("Release", "1")):
            with self.subTest(config=config):
                defs = settings["configs"][config]["GCC_PREPROCESSOR_DEFINITIONS"]
                self.assertIn(f"RMP_PRODUCTION_BUILD={value}", defs)
                self.assertIn("PLATFORM_IOS", defs)
                self.assertIn("GRAPHICS_API_OPENGL_ES3", defs)
                self.assertIn('RMP_RESOURCES_PATH=\\"./resources/\\"', defs)
                self.assertNotIn("DEVELOPMENT_TEAM", settings["configs"][config])

    def test_gradle_properties_are_key_equals_value(self):
        with quiet():
            cfgmod.gen_gradle_properties(base_config(), ["android"])
        path = Path(self._tmp.name) / "raymob" / "generated.properties"
        for line in path.read_text().splitlines():
            if line and not line.startswith("#"):
                self.assertIn("=", line, f"not a property line: {line!r}")


class ConfigureFrozenVersionsTest(unittest.TestCase):
    """The pins are executable documentation; parsing them must not rot."""

    def test_the_block_parses_and_has_the_keys_ci_reads(self):
        pins = cfgmod.frozen_versions()
        for key in ("build_image_digest", "android_ndk", "gradle", "clang_format", "clang_tidy"):
            self.assertIn(key, pins)
        self.assertTrue(pins["build_image_digest"].startswith("sha256:"))


class ConfigureLocateTest(unittest.TestCase):
    """locate(): turning a config key into a line number in the .toml.

    This is what makes an error actionable rather than a scavenger hunt, and it
    is a hand-rolled scan — tomllib throws positions away — so the ways it can
    be wrong are the ordinary ways a scanner is wrong: the same key name in two
    sections, a key inside a comment, a key that is only in DEFAULTS.
    """

    def test_it_finds_a_key_and_returns_its_line(self):
        spot = cfgmod.locate("project", "name")
        self.assertIsNotNone(spot)
        number, text = spot
        self.assertGreater(number, 0)
        self.assertIn("name", text)

    def test_the_same_key_in_two_sections_resolves_to_two_lines(self):
        """[linux] backend and [windows] backend. Getting this wrong points the
        user at somebody else's line, which is worse than no line at all."""
        linux = cfgmod.locate("linux", "backend")
        windows = cfgmod.locate("windows", "backend")
        self.assertIsNotNone(linux)
        self.assertIsNotNone(windows)
        self.assertNotEqual(linux[0], windows[0])

    def test_a_key_the_user_never_wrote_falls_back_to_the_section_header(self):
        """It came from DEFAULTS, so there is no line for it — and pointing at
        the section is the honest answer, not pointing at nothing."""
        spot = cfgmod.locate("project", "a_key_nobody_has_ever_written")
        self.assertIsNotNone(spot)
        self.assertIn("[project]", spot[1])

    def test_an_unknown_section_has_no_line(self):
        self.assertIsNone(cfgmod.locate("nosuchsection", "name"))

    def test_a_commented_out_key_is_not_a_match(self):
        """`# backend = "rgfw"` in the explanation above the real setting is a
        comment, and a scanner that matches it sends you to the wrong line."""
        spot = cfgmod.locate("linux", "backend")
        self.assertIsNotNone(spot)
        self.assertFalse(spot[1].strip().startswith("#"))


class ConfigureErrorLocationTest(unittest.TestCase):
    """locate_from(): every rejection carries its own location, for free.

    Almost every message in configure.py opens with `[section] key`, so the
    location is read back out of the message instead of being threaded through
    sixty raise sites. These are the tests that keep that convention honest.
    """

    def test_it_reads_the_section_and_key_out_of_the_message(self):
        exc = cfgmod.ConfigError("[linux] backend has to be one of glfw, rgfw")
        spot = cfgmod.locate_from(exc)
        self.assertIsNotNone(spot)
        self.assertEqual(spot, cfgmod.locate("linux", "backend"))

    def test_a_nested_section_works_too(self):
        exc = cfgmod.ConfigError("[deploy.itch] user must be a username")
        self.assertEqual(cfgmod.locate_from(exc), cfgmod.locate("deploy.itch", "user"))

    def test_a_section_with_no_key_points_at_the_header(self):
        exc = cfgmod.ConfigError("[targets]: nothing left to build.")
        spot = cfgmod.locate_from(exc)
        self.assertIsNotNone(spot)
        self.assertIn("[targets]", spot[1])

    def test_an_explicit_where_wins_over_the_message(self):
        exc = cfgmod.ConfigError("something about [linux] backend", ("windows", "backend"))
        self.assertEqual(cfgmod.locate_from(exc), cfgmod.locate("windows", "backend"))

    def test_a_message_that_follows_no_convention_simply_has_no_location(self):
        """Better than guessing. A wrong line is worse than none."""
        exc = cfgmod.ConfigError("raylib_multiplatform.toml is not valid TOML")
        self.assertIsNone(cfgmod.locate_from(exc))

    def test_every_validate_rejection_can_be_located(self):
        """The property that matters, checked against the real thing: take every
        way validate() can say no, and assert the reporter can point at a line.

        A rejection nobody can find is the failure this whole mechanism exists to
        prevent, and it is one careless message away at all times.
        """
        broken = [
            ("upx__enabled", "not-a-list"),
            ("upx__max_size_mb", 0),
            ("linux", {"backend": "cocoa", "wayland": False}),
            ("windows", {"backend": "cocoa"}),
            ("web", {"memory": 1, "grow": False}),
            ("window", {"title": "t", "width": 2, "height": 450,
                        "orientation": "landscape"}),
            ("window", {"vsync": "on"}),
            ("window", {"fps": 2000}),
            ("window", {"fps": 15}),   # against the default max_delta 0.05
            ("raylib", {"disabled_modules": ["rshapes"]}),
            ("ui", dict(cfgmod.DEFAULTS["ui"], theme="chartreuse")),
            ("dev", {"compiler": "clang", "linker": "gold"}),
            ("icon", {"source": "x.png", "adaptive_background": "green"}),
        ]
        for key, value in broken:
            with self.subTest(key=key):
                cfg = base_config(**{key: value})
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(cfg, False)
                self.assertIsNotNone(
                    cfgmod.locate_from(caught.exception),
                    f"this rejection cannot be pointed at:\n  {caught.exception}")


class ConfigureWindowPacingTest(unittest.TestCase):
    """[window] vsync and fps: every value they can take, every one they cannot,
    the combination with [app] max_delta that cannot work, and the proof that
    both reach the header and app.cpp -- a setting that validates and never
    reaches raylib is a setting that does nothing."""

    def validate(self, **window):
        cfg = base_config(window=window)
        with quiet():
            cfgmod.validate(cfg, False)

    def reject(self, said, app=None, **window):
        overrides = {"window": window}
        if app is not None:
            overrides["app"] = app
        cfg = base_config(**overrides)
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        self.assertIn(said, str(caught.exception))
        self.assertIsNotNone(cfgmod.locate_from(caught.exception),
                             f"cannot be pointed at: {caught.exception}")
        return str(caught.exception)

    def test_the_defaults_are_vsync_on_and_no_cap(self):
        self.assertIs(cfgmod.DEFAULTS["window"]["vsync"], True)
        self.assertEqual(cfgmod.DEFAULTS["window"]["fps"], 0)

    def test_every_value_they_can_take(self):
        for vsync in (True, False):
            with self.subTest(vsync=vsync):
                self.validate(vsync=vsync)
        # 20 is exactly 1 / 0.05, the default clamp: allowed, it is not LONGER.
        for fps in (0, 20, 30, 60, 144, 240, 1000):
            with self.subTest(fps=fps):
                self.validate(fps=fps)

    def test_vsync_is_a_switch_and_nothing_else(self):
        for bad in ("true", "yes", 1, 0, 1.0, None, [True], {"on": True}):
            with self.subTest(vsync=bad):
                self.reject("[window] vsync", vsync=bad)

    def test_fps_is_a_whole_number(self):
        for bad in (True, False, "60", 60.0, 59.94, None, [60], {"fps": 60}):
            with self.subTest(fps=bad):
                self.reject("whole number of frames per second", fps=bad)

    def test_fps_is_between_0_and_1000(self):
        for bad in (-1, 1001, 2 ** 31):
            with self.subTest(fps=bad):
                self.reject("between 0 and 1000", fps=bad)

    def test_a_cap_longer_than_the_clamp_is_slow_motion(self):
        message = self.reject("75% speed", fps=15)
        self.assertIn("max_delta", message)
        # And it says what to do: three ways out, with the numbers.
        self.assertIn("Raise fps to 20", message)
        self.assertIn("max_delta = 0", message)
        self.assertIn("[window] fps = 15", message)

    def test_the_same_cap_is_fine_without_a_clamp_or_with_a_longer_one(self):
        for max_delta in (0, 0.1):
            with self.subTest(max_delta=max_delta):
                cfg = base_config(window={"fps": 15}, app={"max_delta": max_delta})
                with quiet():
                    cfgmod.validate(cfg, False)

    def test_a_typo_is_refused_by_name(self):
        cfg = copy.deepcopy(cfgmod.DEFAULTS)
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.deep_merge(cfg, {"window": {"vsnyc": True}})
        self.assertIn("vsnyc", str(caught.exception))

    def test_both_reach_the_header(self):
        with generated_header(base_config(window={"vsync": False, "fps": 144})) as header:
            self.assertRegex(header, r"#define RMP_WINDOW_VSYNC +0\n")
            self.assertRegex(header, r"#define RMP_WINDOW_FPS +144\n")
        with generated_header(base_config()) as header:
            self.assertRegex(header, r"#define RMP_WINDOW_VSYNC +1\n")
            self.assertRegex(header, r"#define RMP_WINDOW_FPS +0\n")

    def test_the_toml_documents_both_in_window(self):
        text = (REPO / "raylib_multiplatform.toml").read_text()
        window = text[text.index("\n[window]"):text.index("\n[app]")]
        self.assertRegex(window, r"\nvsync = true\n")
        self.assertRegex(window, r"\nfps = 0\n")

    def test_app_cpp_uses_both_in_the_right_order_and_never_on_the_web(self):
        text = (REPO / "src" / "rmp" / "app.cpp").read_text()
        start = text.index("void start(std::unique_ptr<rmp::Scene> first) {")
        body = text[start:text.index("\n}\n", start)]
        self.assertIn("RMP_WINDOW_VSYNC", body)
        self.assertIn("FLAG_VSYNC_HINT", body)
        guard = body.index("#if !defined(PLATFORM_WEB) && !defined(__EMSCRIPTEN__) "
                           "&& !defined(PLATFORM_IOS)")
        cap = body.index("SetTargetFPS(RMP_WINDOW_FPS)")
        end = body.index("#endif", guard)
        self.assertLess(guard, cap)
        self.assertLess(cap, end)
        # Before the window: raylib's DRM backend reads it to pick the mode.
        self.assertLess(cap, body.index("InitWindow("))

    def test_the_boot_line_reports_it_and_the_render_check_reads_it(self):
        self.assertIn("vsync=%d", (REPO / "tests" / "smoke_test.h").read_text())
        render = (REPO / "tools" / "render_check.sh").read_text()
        self.assertIn("RMP_WINDOW_VSYNC", render)
        self.assertIn("vsync=$WANT_VSYNC", render)


def unguarded_calls(text: str, call: str) -> list[int]:
    """Lines where `call(` appears in code that the web build also compiles.

    A preprocessor walk: each #if/#ifdef/#ifndef/#elif pushes its condition,
    #else negates it, #endif pops. A call is guarded when some condition above
    it excludes the web: `!defined(PLATFORM_WEB)`, `!defined(__EMSCRIPTEN__)`,
    or the #else of a branch that was the web. Comments do not count."""
    stack = []
    bad = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("//", 1)[0].strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind == "if":
                stack.append(rest)
            elif kind == "ifdef":
                stack.append(f"defined({rest})")
            elif kind == "ifndef":
                stack.append(f"!defined({rest})")
            elif kind == "elif" and stack:
                stack[-1] = rest
            elif kind == "else" and stack:
                stack[-1] = f"!({stack[-1]})"
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if f"{call}(" not in line:
            continue
        def excludes_web(cond):
            if "!defined(PLATFORM_WEB)" in cond or "!defined(__EMSCRIPTEN__)" in cond:
                return True
            return cond.startswith("!(") and ("defined(PLATFORM_WEB)" in cond
                                              or "defined(__EMSCRIPTEN__)" in cond)
        if not any(excludes_web(c) for c in stack):
            bad.append(lineno)
    return bad


class NoSetTargetFpsOnTheWebTest(unittest.TestCase):
    """CLAUDE.md: "Do not call SetTargetFPS() on web." The browser owns the frame
    loop there, and a wait inside its callback only stalls it. That was a note,
    and ui/04_settings called SetTargetFPS(60) on every platform anyway, the web
    included -- so it is a check now, over every #if branch."""

    def test_every_call_excludes_the_web(self):
        seen = 0
        for root in ("include", "src", "tests", "examples"):
            for path in sorted((REPO / root).rglob("*")):
                if path.suffix not in (".h", ".c", ".cpp") or "fixtures" in path.parts:
                    continue
                text = path.read_text(errors="replace")
                if "SetTargetFPS(" not in text:
                    continue
                seen += 1
                for lineno in unguarded_calls(text, "SetTargetFPS"):
                    with self.subTest(file=path.relative_to(REPO).as_posix(), line=lineno):
                        self.fail(f"{path.relative_to(REPO)}:{lineno} calls SetTargetFPS "
                                  "where the web build compiles it")
        self.assertGreater(seen, 0, "found no SetTargetFPS at all -- app.cpp has one")

    def test_the_walk_itself(self):
        self.assertEqual(unguarded_calls("SetTargetFPS(60);", "SetTargetFPS"), [1])
        self.assertEqual(unguarded_calls(
            "#if !defined(PLATFORM_WEB) && !defined(PLATFORM_IOS)\nSetTargetFPS(60);\n#endif",
            "SetTargetFPS"), [])
        self.assertEqual(unguarded_calls(
            "#if defined(PLATFORM_WEB)\nx();\n#else\nSetTargetFPS(60);\n#endif",
            "SetTargetFPS"), [])
        self.assertEqual(unguarded_calls(
            "#if defined(PLATFORM_WEB)\nSetTargetFPS(60);\n#endif", "SetTargetFPS"), [2])
        self.assertEqual(unguarded_calls(
            "#ifndef __EMSCRIPTEN__\n#endif\nSetTargetFPS(1);", "SetTargetFPS"), [3])
        self.assertEqual(unguarded_calls("// SetTargetFPS(60);", "SetTargetFPS"), [])


def macro_body(text: str, name: str) -> str:
    """The body of `#define NAME(...)`: every continuation line after it."""
    start = text.index(f"#define {name}(")
    lines = []
    for line in text[start:].splitlines():
        lines.append(line)
        if not line.rstrip().endswith("\\"):
            break
    return "\n".join(lines)


def function_body(text: str, signature: str) -> str:
    """From `signature` to the first closing brace in column 0."""
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


RUNNERS = ("RMP_IOS_FUNCS", "RMP_WEB_FUNCS", "RMP_DESKTOP_FUNCS")


class EscapeIsTheGamesKeyTest(unittest.TestCase):
    """raylib closes the window on Escape unless told otherwise: InitWindow()
    sets the exit key to KEY_ESCAPE, and WindowShouldClose() turns true on the
    press. rmp::ui goes back on Escape and the factory `ui_cancel` action is
    bound to it, so in every game the framework built, the key that meant
    "back" or "pause" closed the game instead.

    No key can be pressed into a headless window, so this checks the call and
    where it is: after the hook that opens the window, on every runner. And
    raylib's own backends, two of which closed on Escape whatever the exit key
    said."""

    APP_H = REPO / "include" / "rmp" / "app.h"
    APP_CPP = REPO / "src" / "rmp" / "app.cpp"

    def test_after_ready_takes_the_exit_key_away_before_anything_can_return(self):
        body = function_body(self.APP_CPP.read_text(), "void after_ready() {")
        self.assertIn("SetExitKey(KEY_NULL);", body)
        code = [line.split("//", 1)[0] for line in body.splitlines()]
        clear = next(i for i, line in enumerate(code) if "SetExitKey(KEY_NULL)" in line)
        returns = [i for i, line in enumerate(code) if re.search(r"\breturn\b", line)]
        self.assertTrue(all(clear < r for r in returns),
                        "SetExitKey(KEY_NULL) has to come before the first return in "
                        "after_ready(): a failed save test returns early")

    def test_every_runner_runs_after_ready_straight_after_the_ready_hook(self):
        """The ready hook is where every window opens -- start() for RMP_GAME,
        the game's own on_ready() for RMP_ENTRY_POINT -- and InitWindow() puts
        Escape back. So after_ready() has to follow it on all three runners."""
        app_h = self.APP_H.read_text()
        for runner in RUNNERS:
            with self.subTest(runner=runner):
                body = macro_body(app_h, runner)
                self.assertRegex(body, r"READY\(\);\s*\\\s*\n\s*rmp::app::detail::after_ready\(\);")

    def test_no_window_of_ours_ever_called_set_exit_key_back(self):
        for root in ("src", "include"):
            for path in sorted((REPO / root).rglob("*")):
                if path.suffix not in (".h", ".cpp", ".c"):
                    continue
                for lineno, line in enumerate(path.read_text().splitlines(), 1):
                    code = line.split("//", 1)[0]
                    if "SetExitKey(" in code and "SetExitKey(KEY_NULL)" not in code:
                        self.fail(f"{path.relative_to(REPO)}:{lineno} gives Escape back to "
                                  "raylib")

    def test_app_h_says_what_a_game_with_its_own_window_gets(self):
        text = self.APP_H.read_text()
        self.assertIn("SetExitKey(KEY_NULL)", text)
        self.assertIn("rmp::app::quit()", text)

    # Every backend a .toml can select, plus SDL, which raylib ships beside them.
    # rcore_memory.c is the one left out: it is the headless test platform,
    # its "keyboard" is the test process's stdin, and nothing ships with it.
    BACKENDS = ("rcore_desktop_glfw.c", "rcore_desktop_rgfw.c", "rcore_desktop_win32.c",
                "rcore_desktop_sdl.c", "rcore_drm.c", "rcore_web.c",
                "rcore_web_emscripten.c", "rcore_android.c")

    def test_no_backend_closes_on_a_key_that_is_not_the_exit_key(self):
        """rcore_desktop_win32.c closed on KEY_ESCAPE by name, and the DRM
        backend's terminal keyboard wrote a lone ESC into the EXIT KEY's slot --
        slot 0 once the exit key is KEY_NULL, which its own exit check then
        read as pressed. Either way SetExitKey() was not obeyed."""
        platforms = REPO / "thirdparty" / "raylib" / "src" / "platforms"
        closing = re.compile(r"shouldClose\s*=\s*true|SetWindowShouldClose\([^)]*TRUE"
                             r"|setShouldClose\([^)]*true")
        for name in self.BACKENDS:
            text = (platforms / name).read_text()
            for lineno, line in enumerate(text.splitlines(), 1):
                code = line.split("//", 1)[0]
                with self.subTest(backend=name, line=lineno):
                    self.assertNotRegex(code, r"currentKeyState\[CORE\.Input\.Keyboard\.exitKey\]\s*=[^=]")
                    if closing.search(code) and re.search(r"\bkey\b|KEY_", code):
                        self.assertIn("exitKey", code,
                                      f"{name}:{lineno} closes the window on a key, and not "
                                      "on the one SetExitKey() named")

    def test_the_scan_sees_the_shape_it_is_looking_for(self):
        closing = re.compile(r"shouldClose\s*=\s*true")
        bad = "if ((key == KEY_ESCAPE) && (state == 1)) CORE.Window.shouldClose = true;"
        self.assertTrue(closing.search(bad) and "exitKey" not in bad)


class InputIsSampledOncePerFrameTest(unittest.TestCase):
    """rmp::input reads devices once per frame, at the frame boundary, and the
    boundary used to be inside rmp::app::detail::frame() -- which only RMP_GAME
    calls. A game written with RMP_ENTRY_POINT and its own on_frame() read
    nothing at all. Sampling twice is as bad: the second sample compares the
    key with itself and every just_pressed() is lost.

    So the boundary belongs to the runner, exactly once, before the frame
    hook, on all three runners -- and nowhere else in the framework.
    tests/input_play.cpp drives both shapes through the real desktop runner
    (`rmp test examples`); this is the half that runs on every `rmp test` and
    sees the iOS and web runners too."""

    APP_H = REPO / "include" / "rmp" / "app.h"
    APP_CPP = REPO / "src" / "rmp" / "app.cpp"

    def test_every_runner_begins_the_frame_once_right_before_the_hook(self):
        app_h = self.APP_H.read_text()
        for runner in RUNNERS:
            with self.subTest(runner=runner):
                body = macro_body(app_h, runner)
                self.assertEqual(body.count("rmp::app::detail::begin_frame();"), 1)
                self.assertRegex(body, r"rmp::app::detail::begin_frame\(\);\s*\\\s*\n\s*"
                                       r"FRAME\(rmp::app::detail::step_delta\(\)\);")

    def test_the_runner_boundary_samples_the_devices(self):
        body = function_body(self.APP_CPP.read_text(), "void begin_frame() {")
        self.assertIn("rmp::input::detail::begin_frame();", body)

    def test_frame_does_not_sample_a_second_time(self):
        body = function_body(self.APP_CPP.read_text(), "void frame(float delta) {")
        code = "\n".join(line.split("//", 1)[0] for line in body.splitlines())
        self.assertNotIn("input::detail::begin_frame", code)

    def test_nothing_else_in_the_framework_samples(self):
        calls = []
        for path in sorted((REPO / "src").rglob("*")):
            if path.suffix not in (".cpp", ".h"):
                continue
            for lineno, line in enumerate(path.read_text().splitlines(), 1):
                if "input::detail::begin_frame()" in line.split("//", 1)[0]:
                    calls.append(f"{path.relative_to(REPO).as_posix()}:{lineno}")
        self.assertEqual(len(calls), 1, calls)
        self.assertTrue(calls[0].startswith("src/rmp/app.cpp:"), calls)

    def test_the_played_check_is_built_and_run(self):
        self.assertIn("tests/input_play.cpp", (REPO / "CMakeLists.txt").read_text())
        script = (REPO / "tools" / "examples_build.sh").read_text()
        self.assertIn("input_play", script)
        self.assertIn("^INPUT PASS$", script)


class ReleaseStartsFromAnyFolderTest(unittest.TestCase):
    """A production build reads "./resources/", and every CI boot started the
    binary where resources/ happened to be -- so a release that read it from
    the working directory, and loaded nothing for a player who started it from
    a file manager or a shortcut, passed every gate. tools/shipped_check.sh is
    the gate that saw it fail (assets_failed=1 from another folder); these say
    that every target that boots a release starts one from somewhere else."""

    WORKFLOWS = REPO / ".github" / "workflows"

    def test_the_desktop_runner_moves_next_to_the_resources_before_opening_them(self):
        body = function_body((REPO / "src" / "rmp" / "app.cpp").read_text(),
                             "void begin_run() {")
        self.assertIn("enter_executable_folder();", body)
        self.assertLess(body.index("enter_executable_folder();"),
                        body.index("rmp::assets::init();"))
        guard = body[:body.index("enter_executable_folder();")]
        self.assertIn("RMP_PRODUCTION_BUILD", guard[guard.rindex("#elif"):])

    def test_a_system_that_cannot_say_where_the_executable_is_is_said_once(self):
        text = (REPO / "src" / "rmp" / "app.cpp").read_text()
        body = function_body(text, "void enter_executable_folder() {")
        self.assertIn("this system does not say where the executable is", body)
        self.assertNotIn("RMP_REPORT_ONCE", body)
        script = (REPO / "tools" / "shipped_check.sh").read_text()
        self.assertIn("this system does not say where the executable is", script)

    def test_linux_starts_the_unzipped_archive_from_another_folder(self):
        text = (self.WORKFLOWS / "_linux.yml").read_text()
        for target in ("linux-x64-glibc", "linux-arm64-glibc"):
            with self.subTest(target=target):
                self.assertIn(f'unzip -q {target}-build.zip -d "$SHIP"', text)
        self.assertEqual(text.count("- name: The archive starts from any folder"), 2)
        self.assertEqual(text.count('          cd "$ELSEWHERE"\n          RAY_TEST_MAX_FRAMES=10'), 2)

    def test_the_containers_start_the_release_from_the_root(self):
        text = (self.WORKFLOWS / "_linux.yml").read_text()
        self.assertNotIn("-w /app", text)
        self.assertEqual(text.count("            -w / -e RAY_TEST_MAX_FRAMES=10"), 2)
        self.assertEqual(text.count('"/app/${{ inputs.project_name }}" 2>&1'), 2)

    def test_windows_starts_the_exe_from_another_folder_next_to_its_pack(self):
        text = (self.WORKFLOWS / "_windows.yml").read_text()
        self.assertIn("Copy-Item resources/resources.rres build/release/resources/", text)
        self.assertIn("Start-Process -FilePath $exe -WorkingDirectory $elsewhere", text)

    def test_macos_and_the_bsds_run_the_shipped_check(self):
        self.assertIn('sh tools/shipped_check.sh Ninja "" "${{ inputs.project_name }}"',
                      (self.WORKFLOWS / "_apple.yml").read_text())
        bsd = (self.WORKFLOWS / "_bsd.yml").read_text()
        self.assertIn('sh tools/shipped_check.sh "$GENERATOR" "$EXTRA_CMAKE"', bsd)
        self.assertLess(bsd.index("tools/shipped_check.sh"), bsd.index("tools/render_check.sh"))

    def test_the_check_starts_it_from_a_folder_with_no_resources(self):
        script = (REPO / "tools" / "shipped_check.sh").read_text()
        self.assertIn('cd "$BUILD/elsewhere"\nRAY_TEST_MAX_FRAMES=5 "$ROOT/$SHIP/$NAME"', script)
        self.assertIn('grep -q "RAY_TEST_BOOT_OK assets_failed=0 " "$LOG"', script)
        self.assertIn("-DPRODUCTION_BUILD=ON", script)

    def test_the_check_starts_it_from_the_project_folder_too(self):
        """Where `rmp build release` leaves it: build/release/<name>, resources/
        at the root, started from the root. It must not move. examples/plain_c moved
        into build/ in every desktop release and loaded nothing from there
        (assets_failed=1); this case is the one that said so."""
        script = (REPO / "tools" / "shipped_check.sh").read_text()
        root_case = script.index('RAY_TEST_MAX_FRAMES=5 "./$BUILD/$NAME"')
        self.assertLess(root_case, script.index('case "$(uname -s)" in'))
        self.assertIn('grep -q "RAY_TEST_BOOT_OK assets_failed=0 " "$LOG"',
                      script[root_case:script.index("from the project folder it reads")])


def boot_checks(path: Path) -> list[tuple[int, str]]:
    """Every line of code in `path` that looks for RAY_TEST_BOOT_OK. Comments
    are not checks: a `#` line in YAML or shell, a `//` line in JavaScript."""
    found = []
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        code = line.strip()
        if code.startswith("#") or code.startswith("//"):
            continue
        if "RAY_TEST_BOOT_OK" in code:
            found.append((lineno, code))
    return found


class EveryBootRequiresItsAssetsTest(unittest.TestCase):
    """RAY_TEST_BOOT_OK says the window opened. It does not say the game found
    what it ships: the line carries assets_failed=N, and a boot with every
    texture missing still prints RAY_TEST_BOOT_OK. The Linux, Windows and iOS
    boots and the examples required assets_failed=0; render_check.sh (macOS,
    the BSDs, the lint job, every `rmp new` game), the web boot and the musl
    and DRM runs grepped the marker alone. So every place a boot is checked
    has to require assets_failed=0 on the same line."""

    def sources(self):
        roots = [REPO / ".github" / "workflows", REPO / ".github" / "scripts", REPO / "tools"]
        for root in roots:
            for path in sorted(root.iterdir()):
                if path.suffix in (".yml", ".yaml", ".sh", ".py", ".js"):
                    yield path

    def test_every_boot_check_requires_assets_failed_0(self):
        seen = 0
        for path in self.sources():
            for lineno, code in boot_checks(path):
                seen += 1
                with self.subTest(at=f"{path.relative_to(REPO).as_posix()}:{lineno}"):
                    self.assertIn("RAY_TEST_BOOT_OK assets_failed=0 ", code,
                                  "a boot check that does not require assets_failed=0 passes "
                                  "a game that loaded none of its assets")
        # The Linux, Windows, iOS, web, musl and DRM boots, render_check.sh,
        # shipped_check.sh, examples_build.sh and rmp.py's smoke, at least.
        self.assertGreaterEqual(seen, 12, "the scan found almost no boot checks")

    def test_the_scan_tells_a_check_from_a_comment(self):
        sample = REPO / "build" / "boot_checks_sample.sh"
        sample.parent.mkdir(exist_ok=True)
        sample.write_text('# RAY_TEST_BOOT_OK in a comment\n'
                          'grep -q RAY_TEST_BOOT_OK "$LOG"\n'
                          '  // RAY_TEST_BOOT_OK in a JS comment\n')
        try:
            self.assertEqual([n for n, _ in boot_checks(sample)], [2])
        finally:
            sample.unlink()


class ConfigureAdmobIdsTest(unittest.TestCase):
    """[android.admob] app_id, interstitial_id and rewarded_id go into the
    manifest and into the SDK as they are written. `app_id = 5` and
    `app_id = ""` passed validate(): the first reached Gradle as "5", the
    second as nothing, and the Google Mobile Ads SDK stops an app at startup
    over an app id it cannot read. Each one is the AdMob shape:

        app id      ca-app-pub-<16 digits>~<10 digits>
        ad unit id  ca-app-pub-<16 digits>/<10 digits>

    which Google's own test ids, the ones raylib_multiplatform.toml ships
    with, are. The type is checked always; the shape while AdMob is on, since
    an id nothing reads is not wrong."""

    KEYS = ("app_id", "interstitial_id", "rewarded_id")

    def config(self, enabled=True, **ids):
        cfg = base_config()
        cfg["android"]["admob"] = {**cfg["android"]["admob"], "enabled": enabled, **ids}
        return cfg

    def reject(self, needle, **kw):
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(self.config(**kw), False)
        message = str(caught.exception)
        self.assertIn(needle, message)
        self.assertIsNotNone(cfgmod.locate_from(caught.exception),
                             f"cannot be pointed at: {message}")
        return message

    def test_googles_test_ids_are_the_defaults_and_they_pass(self):
        admob = cfgmod.DEFAULTS["android"]["admob"]
        self.assertEqual(admob["app_id"], "ca-app-pub-3940256099942544~3347511713")
        self.assertEqual(admob["interstitial_id"], "ca-app-pub-3940256099942544/1033173712")
        self.assertEqual(admob["rewarded_id"], "ca-app-pub-3940256099942544/5224354917")
        with quiet():
            cfgmod.validate(self.config(), False)

    def test_the_shipped_toml_carries_the_same_test_ids(self):
        text = (REPO / "raylib_multiplatform.toml").read_text()
        admob = cfgmod.DEFAULTS["android"]["admob"]
        for key in self.KEYS:
            with self.subTest(key=key):
                self.assertRegex(text, rf'\n{key} *= "{re.escape(admob[key])}"\n')

    def test_ids_of_the_right_shape_pass(self):
        with quiet():
            cfgmod.validate(self.config(app_id="ca-app-pub-1234567890123456~0987654321",
                                        interstitial_id="ca-app-pub-1234567890123456/1111111111",
                                        rewarded_id="ca-app-pub-1234567890123456/2222222222"),
                            False)

    def test_the_wrong_type_is_refused_on_or_off(self):
        for key in self.KEYS:
            for bad in (5, True, 1.5, ["ca-app-pub-3940256099942544~3347511713"], {"id": 1}):
                for enabled in (True, False):
                    with self.subTest(key=key, value=bad, enabled=enabled):
                        message = self.reject(f"[android.admob] {key}", enabled=enabled,
                                              **{key: bad})
                        self.assertIn("has to be a string", message)

    def test_empty_is_refused_while_admob_is_on(self):
        for key in self.KEYS:
            with self.subTest(key=key):
                message = self.reject(f"[android.admob] {key} is empty", **{key: ""})
                self.assertIn("enabled = false", message)

    def test_an_id_of_the_wrong_shape_is_refused_while_admob_is_on(self):
        bad = ("5", "pub-3940256099942544~3347511713", "ca-app-pub-394025609994254~3347511713",
               "ca-app-pub-3940256099942544~334751171", " ca-app-pub-3940256099942544~3347511713",
               "ca-app-pub-3940256099942544~3347511713\n", "ca-app-pub-39402565099942544~33475117130")
        for value in bad:
            with self.subTest(value=value):
                message = self.reject("is not an AdMob app id", app_id=value)
                self.assertIn("ca-app-pub-<16 digits>~<10 digits>", message)

    def test_the_app_id_and_a_unit_id_swapped_say_so(self):
        message = self.reject("is not an AdMob app id",
                              app_id="ca-app-pub-3940256099942544/1033173712")
        self.assertIn("ad unit id", message)
        for key in ("interstitial_id", "rewarded_id"):
            with self.subTest(key=key):
                message = self.reject("is not an AdMob ad unit id",
                                      **{key: "ca-app-pub-3940256099942544~3347511713"})
                self.assertIn("the app id", message)
                self.assertIn("ca-app-pub-<16 digits>/<10 digits>", message)

    def test_off_means_the_shape_is_not_read(self):
        with quiet():
            cfgmod.validate(self.config(enabled=False, app_id="", interstitial_id="x",
                                        rewarded_id=""), False)


class TechnicalSignaturesTest(unittest.TestCase):
    """TECHNICAL.md's "What you actually get" listed `Texture2D
    load_texture(const char *)` and `load_data(const char *, int *)` long
    after both had become counted handles taking std::string_view. A
    signature in the notes is checked against the header it describes."""

    def test_every_listed_loader_is_declared_so_in_assets_h(self):
        text = (REPO / "TECHNICAL.md").read_text()
        start = text.index("### What you actually get")
        block = text[text.index("```cpp", start) + 6:text.index("```", text.index("```cpp", start) + 6)]
        header = re.sub(r"\s+", " ", (REPO / "include" / "rmp" / "assets.h").read_text())
        seen = 0
        for line in block.splitlines():
            line = line.split("//", 1)[0].strip()
            match = re.match(r"(.+?)\s+rmp::assets::(\w+)\((.*)\);$", line)
            if not match:
                continue
            seen += 1
            kind, name, params = match.groups()
            with self.subTest(name=name):
                kind = re.sub(r"\s+", " ", kind)
                declared = f"{kind} {name}({params});"
                self.assertIn(declared, header)
        self.assertGreaterEqual(seen, 7)


class EveryOptionRefusesTheWrongTypeTest(unittest.TestCase):
    """CLAUDE.md: for every option, the wrong TYPE and the EMPTY value. The
    membership table above was a list somebody kept, and it missed three keys:
    `orientation = ["landscape"]`, `gl_version = ["ES30"]` and `linker =
    ["lld"]` each answered with a Python traceback, and `[icon] source` was
    never checked at all -- `source = 5` passed validate() and died in
    generate_icons(). So this walks DEFAULTS itself: every key there is, now
    and when one is added, gets every type it is not, and must be refused with
    a ConfigError that names its line -- through validate() and on through the
    generators, so a value validate() lets past cannot crash them either."""

    # Keys whose "" is a value, and what it means. Every other string key
    # refuses "".
    EMPTY_STRING_MEANS = {
        ("icon", "adaptive_background"): "no background colour",
        ("linux", "glibc"): "build against the host's glibc",
        ("ui", "font"): "raylib's built-in font",
        ("deploy", "credits_note"): "nothing to say while licenses = true",
        ("deploy", "itch", "user"): "itch.io deployment off",
        ("deploy", "itch", "game"): "itch.io deployment off",
        ("deploy", "firebase", "project_id"): "Firebase Test Lab off",
        ("deploy", "firebase", "device"): "the workflow's default device",
        # Read only while [android.admob] enabled = true; ConfigureAdmobIdsTest
        # refuses them empty when it is.
        ("android", "admob", "app_id"): "AdMob off",
        ("android", "admob", "interstitial_id"): "AdMob off",
        ("android", "admob", "rewarded_id"): "AdMob off",
    }
    # Lists whose [] is a value. Every other list refuses [].
    EMPTY_LIST_MEANS = {
        ("targets", "disabled"): "nothing subtracted",
        ("upx", "enabled"): "no compression",
        ("upx", "disabled"): "nothing subtracted",
        ("raylib", "disabled_modules"): "every module",
        ("dev", "sanitize"): "a Debug build with no sanitizer",
    }
    # A free-form passthrough: its keys are Xcode's, not ours.
    SKIP = {("ios", "settings")}
    # Whole-number defaults that are numbers, not counts: 1.5 is a scale.
    NUMBERS = {("ui", "scale")}

    WRONG = {"a list": ["x"], "a table": {"x": 1}, "a bool": True, "an int": 5,
             "a float": 1.5, "a string": "x"}

    def leaves(self, node=None, path=()):
        node = cfgmod.DEFAULTS if node is None else node
        for key, value in node.items():
            here = path + (key,)
            if here in self.SKIP:
                continue
            if isinstance(value, dict):
                yield from self.leaves(value, here)
            else:
                yield here, value

    def config_with(self, path, value):
        cfg = base_config()
        node = cfg
        for step in path[:-1]:
            node = node[step]
        node[path[-1]] = copy.deepcopy(value)
        return cfg

    def outcome(self, cfg):
        """None when refused at a line; otherwise what happened instead."""
        original = cfgmod.write
        cfgmod.write = lambda path, content: None
        try:
            with quiet(), contextlib.redirect_stdout(io.StringIO()):
                cfgmod.validate(cfg, False)
                targets = cfgmod.expand_targets(cfg["targets"]["enabled"],
                                                cfg["targets"]["disabled"])
                cfgmod.expand_upx(cfg, targets)
                cfgmod.app_defines(cfg)
                cfgmod.gen_cmake(cfg)
                cfgmod.gen_app_config(cfg)
                cfgmod.gen_gradle_properties(cfg, targets)
                cfgmod.gen_android_manifest(cfg, targets)
                cfgmod.gen_ios_project(cfg)
            return "accepted"
        except cfgmod.ConfigError as caught:
            return None if cfgmod.locate_from(caught) is not None else f"no line: {caught}"
        except Exception as crash:  # noqa: BLE001 -- the point is to see it
            return f"crashed with {type(crash).__name__}: {crash}"
        finally:
            cfgmod.write = original

    @staticmethod
    def same_kind(default, value):
        if isinstance(default, bool) or isinstance(value, bool):
            return type(default) is type(value)
        if isinstance(default, float):
            return isinstance(value, (int, float))  # a whole number is a number
        return type(default) is type(value)

    def test_every_option_refuses_every_type_it_is_not(self):
        checked = 0
        for path, default in self.leaves():
            for label, value in self.WRONG.items():
                if self.same_kind(default, value):
                    continue
                if path in self.NUMBERS and isinstance(value, float):
                    continue
                checked += 1
                with self.subTest(option=".".join(path), value=label):
                    self.assertIsNone(self.outcome(self.config_with(path, value)))
        self.assertGreater(checked, 300, "the walk found almost nothing to check")

    def test_every_option_refuses_the_empty_value_unless_it_means_something(self):
        for path, default in self.leaves():
            if isinstance(default, str) and path not in self.EMPTY_STRING_MEANS:
                with self.subTest(option=".".join(path)):
                    self.assertIsNone(self.outcome(self.config_with(path, "")))
            if isinstance(default, list) and path not in self.EMPTY_LIST_MEANS:
                with self.subTest(option=".".join(path)):
                    self.assertIsNone(self.outcome(self.config_with(path, [])))

    def test_the_empty_values_that_mean_something_are_accepted(self):
        for path in list(self.EMPTY_STRING_MEANS) + list(self.EMPTY_LIST_MEANS):
            empty = [] if path in self.EMPTY_LIST_MEANS else ""
            cfg = self.config_with(path, empty)
            if path[:2] == ("android", "admob"):
                cfg["android"]["admob"]["enabled"] = False
            with self.subTest(option=".".join(path)):
                self.assertEqual(self.outcome(cfg), "accepted")

    def test_the_lists_name_real_keys(self):
        keys = {path for path, _ in self.leaves()}
        for path in list(self.EMPTY_STRING_MEANS) + list(self.EMPTY_LIST_MEANS):
            with self.subTest(option=".".join(path)):
                self.assertIn(path, keys)


def toml_section(name: str) -> str:
    """The text of `[name]` in the shipped .toml, comments included."""
    text = (REPO / "raylib_multiplatform.toml").read_text()
    start = text.index(f"\n[{name}]\n")
    end = text.find("\n[", start + 1)
    return text[start:] if end < 0 else text[start:end]


def listed_names(section: str, label: str) -> list[str]:
    """The names a `#   label : a b c` comment lists, continuation lines too."""
    lines = section.splitlines()
    first = next(i for i, line in enumerate(lines) if re.match(rf"#\s+{label}\s*:", line))
    names = lines[first].split(":", 1)[1].split()
    for line in lines[first + 1:]:
        if not re.match(r"#\s{4,}\S", line) or ":" in line:
            break
        names += line[1:].split()
    return [n for n in names if n != "|"]


class TomlCommentsAreTrueTest(unittest.TestCase):
    """raylib_multiplatform.toml is copied into every game, and its comments
    are what a game's author reads. Each one that lists, claims or promises
    something is held to the code here."""

    def test_the_upx_exact_names_are_the_compressible_targets(self):
        """It left out linux-x64-musl and linux-arm64-glibc-drm."""
        self.assertEqual(sorted(listed_names(toml_section("upx"), "exact")),
                         sorted(cfgmod.UPX_TARGETS))
        self.assertEqual(sorted(listed_names(toml_section("upx"), "groups")),
                         sorted(cfgmod.UPX_GROUPS))

    def test_the_targets_exact_names_are_the_targets(self):
        self.assertEqual(sorted(listed_names(toml_section("targets"), "exact")),
                         sorted(cfgmod.TARGETS))
        self.assertEqual(sorted(listed_names(toml_section("targets"), "groups")),
                         sorted(cfgmod.GROUPS))

    def test_the_raylib_modules_listed_are_the_ones_that_can_go(self):
        """It offered rshapes, which configure.py refuses: rmp::ui draws with it."""
        section = toml_section("raylib")
        listed = re.search(r"#\s+([a-z]+(?:\s*\|\s*[a-z]+)+)\n", section).group(1)
        self.assertEqual(sorted(re.split(r"\s*\|\s*", listed)),
                         sorted(cfgmod.OPTIONAL_MODULES))
        readme = (REPO / "README.md").read_text()
        for module in cfgmod.PROTECTED_MODULES:
            with self.subTest(module=module):
                self.assertNotRegex(readme, rf"disabled_modules = \[\].*{module}")
        self.assertNotIn("`rshapes`, `rmodels` and `raudio` can go", readme)

    def test_orientation_says_what_reads_it(self):
        """It said that on desktop it decides the initial window shape. Only
        the Android manifest and the iOS spec read it."""
        window = toml_section("window")
        note = window[window.index('# "landscape"'):window.index("\norientation =")]
        self.assertNotIn("initial window shape", note)
        self.assertIn("Android", note)
        self.assertIn("iOS", note)
        generated = (REPO / "tools" / "configure.py").read_text()
        self.assertNotIn("RMP_WINDOW_ORIENTATION", generated)

    def test_upx_says_which_packed_binaries_ci_starts(self):
        """It said every push starts the packed binary; the boots ran before
        UPX packed it, and arm64 runs only on a full run. And that
        linux-x64-glibc-drm is cross-compiled and never executed: it is built
        natively and booted on vkms."""
        section = toml_section("upx")
        self.assertNotIn("every push starts the packed\n# binary", section)
        self.assertNotIn("nothing ever executes them", section)
        linux = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        x64 = linux[linux.index("\n  x64:\n"):linux.index("\n  arm64:\n")]
        self.assertLess(x64.index("- name: Package"),
                        x64.index("- name: The archive starts from any folder"),
                        "the packed binary is the one started, after Package")

    def test_no_comment_promises_or_retells(self):
        text = (REPO / "raylib_multiplatform.toml").read_text()
        for said in ("flip this default when we bump", "It used to build Wayland ALONE",
                     "all four", "a default later", "Only x64/glibc is built today"):
            with self.subTest(said=said):
                self.assertNotIn(said, text)


def tiny_png(side: int) -> bytes:
    """A real, square, grey PNG of `side` pixels, with the standard library."""
    import struct
    import zlib

    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    rows = b"".join(b"\0" + b"\x80" * side for _ in range(side))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


@contextlib.contextmanager
def without_pillow():
    """As if Pillow were not installed: `from PIL import ...` raises ImportError."""
    saved = {name: module for name, module in sys.modules.items()
             if name == "PIL" or name.startswith("PIL.")}
    for name in saved:
        del sys.modules[name]
    sys.modules["PIL"] = None
    try:
        yield
    finally:
        del sys.modules["PIL"]
        sys.modules.update(saved)


@contextlib.contextmanager
def repo_at(path: Path):
    """configure.py's REPO pointed at a scratch tree, so nothing it writes
    lands in the checkout."""
    original = cfgmod.REPO
    cfgmod.REPO = path
    try:
        yield
    finally:
        cfgmod.REPO = original


class ConfigureMessagesSayWhatHappenedTest(unittest.TestCase):

    def test_a_hyphen_in_the_application_id_is_named(self):
        """`com.my-game.app` was answered with "needs at least one dot and each
        segment must start with a letter" -- true of it, and not why it was
        refused. Apple takes a hyphen in a bundle id and Android does not, and
        the message says that, with the id spelled the way Android takes it."""
        cfg = base_config()
        cfg["android"]["application_id"] = "com.my-game.app"
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        message = str(caught.exception)
        self.assertIn("'-'", message)
        self.assertIn("com.my_game.app", message)
        self.assertIsNotNone(cfgmod.locate_from(caught.exception))

    def test_other_characters_are_named_too(self):
        for appid, named in (("com.my game.app", "' '"), ("com.mygame!.app", "'!'"),
                             ("com.jögo.app", "'ö'")):
            with self.subTest(appid=appid):
                cfg = base_config()
                cfg["android"]["application_id"] = appid
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(cfg, False)
                self.assertIn(named, str(caught.exception))

    def test_the_pillow_warning_names_the_configured_icon(self):
        """It said resources/icon.png whatever [icon] source said -- and the
        default is branding/icon.png, so it named a file that does not exist."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "art").mkdir()
            (root / "art" / "logo.png").write_bytes(tiny_png(4))
            cfg = base_config()
            cfg["icon"]["source"] = "art/logo.png"
            with repo_at(root), without_pillow(), quiet():
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    cfgmod.generate_icons(cfg, required=True)
            message = str(caught.exception)
            self.assertIn("art/logo.png", message)
            self.assertNotIn("resources/icon.png", message)

    def test_make_default_icon_without_pillow_is_a_message(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with repo_at(root), without_pillow(), quiet():
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    cfgmod.make_default_icon(root / "branding" / "icon.png")
            self.assertIn("pip install pillow", str(caught.exception))
            self.assertFalse((root / "branding" / "icon.png").exists())


class DebugApkShowsTestAdsTest(unittest.TestCase):
    """The debug APK shows Google's test ads, whatever ids the .toml carries.

    _firebase.yml sends the debug APK to Firebase Test Lab, whose Robo crawler
    taps whatever is on the screen, and said the debug build used Google's
    test ad units -- while build.gradle set the .toml's ids in defaultConfig,
    for every build type. A game with its real ids would have had the crawler
    clicking its real ads, which AdMob counts as invalid traffic. Nothing here
    can run Gradle (there is no Android SDK outside the build image), so this
    reads build.gradle: the debug build type overrides all three ids with the
    test ones, the test ones are configure.py's defaults, release overrides
    none, and defaultConfig still takes the .toml's."""

    GRADLE = REPO / "raymob" / "app" / "build.gradle"

    @staticmethod
    def body(text, opener):
        """What is between `opener {` and its matching brace."""
        start = re.search(rf"(?m)^\s*{re.escape(opener)}\s*\{{", text)
        assert start, f"no `{opener} {{` block"
        depth, i = 1, start.end()
        while depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        return text[start.end():i - 1]

    def gradle(self):
        return self.GRADLE.read_text()

    def test_the_test_ids_in_gradle_are_googles(self):
        table = self.gradle().split("def googleTestAds = [", 1)[1].split("]", 1)[0]
        admob = cfgmod.DEFAULTS["android"]["admob"]
        self.assertEqual(dict(re.findall(r"(\w+): '([^']+)'", table)),
                         {key: admob[key] for key in ConfigureAdmobIdsTest.KEYS})

    def test_the_debug_build_type_overrides_every_id(self):
        debug = self.body(self.body(self.gradle(), "buildTypes"), "debug")
        self.assertIn('buildConfigField("String", "ADMOB_INTERSTITIAL_ID", '
                      '"\\"${googleTestAds.interstitial_id}\\"")', debug)
        self.assertIn('buildConfigField("String", "ADMOB_REWARDED_ID", '
                      '"\\"${googleTestAds.rewarded_id}\\"")', debug)
        self.assertIn("manifestPlaceholders['ADMOB_APP_ID'] = googleTestAds.app_id", debug)

    def test_release_keeps_the_tomls_ids(self):
        gradle = self.gradle()
        self.assertNotIn("ADMOB", self.body(self.body(gradle, "buildTypes"), "release"))
        default = self.body(gradle, "defaultConfig")
        for key, name in (("interstitial_id", "ADMOB_INTERSTITIAL_ID"),
                          ("rewarded_id", "ADMOB_REWARDED_ID"), ("app_id", "ADMOB_APP_ID")):
            with self.subTest(key=key):
                self.assertRegex(default, rf"{name}.*project\.properties\['admob\.{key}'\]")

    def test_test_lab_gets_the_debug_apk_and_says_why(self):
        firebase = (REPO / ".github" / "workflows" / "_firebase.yml").read_text()
        for needle in ("name: android-debug-apk", "refusing to run Test Lab on a non-debug APK",
                       "the debug build type in raymob/app/build.gradle"):
            with self.subTest(needle=needle):
                self.assertTrue(needle in firebase, f"_firebase.yml lost {needle!r}")


class AndroidGlVersionTest(unittest.TestCase):
    """[android] gl_version only changed the manifest. raylib on Android was
    always compiled for OpenGL ES 2.0 -- its CMake sets GRAPHICS_API_OPENGL_ES2
    for the platform -- and the -DGL_VERSION Gradle passed was read by nothing,
    so the default ES30 did one thing: it hid the game from every ES 2.0 device
    on the Play Store. Now the value is what raylib and the game are compiled
    for, and what the manifest asks for, and nothing else."""

    GL = REPO / "raymob" / "app" / "src" / "main" / "cpp" / "gl_version.cmake"

    def config(self, value):
        return base_config(android=dict(copy.deepcopy(cfgmod.DEFAULTS["android"]),
                                        gl_version=value))

    def test_the_default_is_es20_the_widest_set_of_devices(self):
        """A game starts with ES20 (DEFAULTS, and `rmp new` writes it --
        tests/rmp_test.py). The framework's own .toml has ES30 so that its
        Android job compiles the ES 3.0 path, the one that is new."""
        self.assertEqual(cfgmod.DEFAULTS["android"]["gl_version"], "ES20")
        self.assertRegex(toml_section("android"), r'\ngl_version = "ES30"\n')

    def test_es20_and_es30_are_the_values(self):
        self.assertEqual(cfgmod.GL_VERSIONS, {"ES20", "ES30"})
        for value in ("ES20", "ES30"):
            with self.subTest(gl_version=value), quiet():
                cfgmod.validate(self.config(value), False)

    def test_es31_and_es32_are_refused_with_the_reason(self):
        """raylib has an ES 3.0 path and no 3.1 or 3.2 one: asking for those
        would compile ES 3.0 and only hide the game from more devices."""
        for value in ("ES31", "ES32"):
            with self.subTest(gl_version=value):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.config(value), False)
                self.assertIn("ES 3.0", str(caught.exception))
                self.assertIsNotNone(cfgmod.locate_from(caught.exception))

    def test_the_value_reaches_gradle(self):
        for value in ("ES20", "ES30"):
            captured = {}
            original = cfgmod.write
            cfgmod.write = lambda path, content: captured.__setitem__(str(path), content)
            try:
                with quiet():
                    cfgmod.gen_gradle_properties(self.config(value), ["android"])
            finally:
                cfgmod.write = original
            (text,) = captured.values()
            with self.subTest(gl_version=value):
                self.assertIn(f"gl.version={value}\n", text)

    def test_gradle_asks_the_manifest_for_the_same_version(self):
        gradle = (REPO / "raymob" / "app" / "build.gradle").read_text()
        table = gradle[gradle.index("def versionCodes = ["):]
        table = table[:table.index("]")]
        self.assertEqual(dict(re.findall(r"'(ES\d\d)': '(0x[0-9a-f]+)'", table)),
                         {"ES20": "0x00020000", "ES30": "0x00030000"})
        self.assertIn('"-DGL_VERSION=$glVersion"', gradle)
        self.assertIn("project.findProperty('gl.version') ?: 'ES20'", gradle)

    def resolve(self, value):
        import shutil
        import subprocess
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake not installed")
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "probe.cmake"
            script.write_text(
                f'include("{self.GL.as_posix()}")\n'
                f'rmp_android_gl("{value}" OPENGL GLES DEFINE)\n'
                'message(STATUS "OPENGL=${OPENGL}|GLES=${GLES}|DEFINE=${DEFINE}")\n')
            got = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
            return got.returncode, got.stdout + got.stderr

    def test_the_native_build_compiles_raylib_and_the_game_for_it(self):
        """What raymob's CMakeLists.txt does with -DGL_VERSION: raylib's own
        OPENGL_VERSION, the GRAPHICS_API_* the game is compiled with, and the
        GLES library the .so links -- ES 3.0's functions are in libGLESv3."""
        for value, opengl, gles, define in (
                ("ES20", "ES 2.0", "GLESv2", "GRAPHICS_API_OPENGL_ES2"),
                ("ES30", "ES 3.0", "GLESv3", "GRAPHICS_API_OPENGL_ES3")):
            with self.subTest(gl_version=value):
                code, out = self.resolve(value)
                self.assertEqual(code, 0, out)
                self.assertIn(f"OPENGL={opengl}|GLES={gles}|DEFINE={define}", out)
        code, out = self.resolve("ES31")
        self.assertNotEqual(code, 0, "an unknown value has to stop the Android build")
        cmakelists = (self.GL.parent / "CMakeLists.txt").read_text()
        uses = cmakelists.index("rmp_android_gl(")
        self.assertLess(uses, cmakelists.index("add_subdirectory(${RAYLIB_DIR}"),
                        "raylib reads OPENGL_VERSION when it is configured")
        self.assertIn("${RMP_GL_DEFINE}", cmakelists)
        self.assertIn("${RMP_GL_LIBRARY}", cmakelists)

    def test_raylib_asks_egl_for_the_context_it_was_compiled_for(self):
        """rcore_android.c asked for EGL_CONTEXT_CLIENT_VERSION 2 whatever it
        was compiled for, and a driver may then hand an ES3 build an ES 2.0
        context to call ES 3.0 functions in."""
        text = (REPO / "thirdparty" / "raylib" / "src" / "platforms" / "rcore_android.c").read_text()
        self.assertIn("EGL_CONTEXT_CLIENT_VERSION, (rlGetVersion() == RL_OPENGL_ES_30)? 3 : 2,",
                      text)


class EntryPointGuardTest(unittest.TestCase):
    """rmp/app.h promises that a program with NO entry point gets a link error
    naming rmp_entry_point_is_declared_exactly_once, and one with TWO gets it
    named twice. The first half never fired: src/rmp/app.cpp referenced the
    symbol through a constant pointer nothing read, the compiler dropped it --
    `nm` on app.cpp.o showed no reference at all -- and the program got the
    "undefined reference to `main`" the guard exists to replace. This compiles
    app.cpp and links it the way an executable is linked, dead sections
    discarded, and reads what the linker says."""

    @classmethod
    def setUpClass(cls):
        import shutil
        cls.cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        cls.nm = shutil.which("nm")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls._tmp.name)
        # The generated header, written here rather than read from the
        # checkout, which a fresh clone does not have until a configure.
        with generated_header(base_config()) as text:
            (cls.tmp / "rmp" / "generated").mkdir(parents=True)
            (cls.tmp / "rmp" / "generated" / "config.h").write_text(text)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_tool(self, argv):
        import subprocess
        return subprocess.run(argv, capture_output=True, text=True)

    def compile(self, source: Path, name: str) -> Path:
        if self.cxx is None or sys.platform == "win32":
            self.skipTest("no C++ compiler that links ELF or Mach-O here")
        out = self.tmp / f"{name}.o"
        got = self.run_tool([
            self.cxx, "-std=c++20", "-O2", "-ffunction-sections", "-fdata-sections",
            "-c", str(source), "-o", str(out),
            "-DRMP_PRODUCTION_BUILD=1", '-DRMP_RESOURCES_PATH="./resources/"',
            f"-I{self.tmp}", f"-I{REPO / 'include'}", f"-I{REPO / 'thirdparty' / 'raylib' / 'src'}",
            f"-I{REPO / 'tests'}", f"-I{REPO / 'thirdparty'}", f"-I{REPO / 'thirdparty' / 'clay'}",
            f"-I{REPO / 'thirdparty' / 'rres'}", f"-I{REPO / 'thirdparty' / 'cJSON'}"])
        self.assertEqual(got.returncode, 0, got.stderr[-2000:])
        return out

    def link(self, *objects):
        dead = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        got = self.run_tool([self.cxx, *map(str, objects), dead, "-o", str(self.tmp / "program")])
        return got.returncode, got.stdout + got.stderr

    def test_app_cpp_asks_for_the_symbol(self):
        app = self.compile(REPO / "src" / "rmp" / "app.cpp", "app")
        if self.nm is None:
            self.skipTest("nm not installed")
        undefined = self.run_tool([self.nm, "-u", str(app)]).stdout
        self.assertRegex(undefined, r"\b_?rmp_entry_point_is_declared_exactly_once\b")

    def test_a_program_with_no_entry_point_is_told_its_name(self):
        code, said = self.link(self.compile(REPO / "src" / "rmp" / "app.cpp", "app"))
        self.assertNotEqual(code, 0)
        self.assertIn("rmp_entry_point_is_declared_exactly_once", said)

    def test_a_program_with_two_entry_points_is_told_its_name(self):
        first = self.tmp / "first.cpp"
        first.write_text("#include <rmp/app.h>\nRMP_DECLARE_ENTRY_POINT_ONCE\n"
                         "int main() { return 0; }\n")
        second = self.tmp / "second.cpp"
        second.write_text("#include <rmp/app.h>\nRMP_DECLARE_ENTRY_POINT_ONCE\n")
        code, said = self.link(self.compile(first, "first"), self.compile(second, "second"))
        self.assertNotEqual(code, 0)
        self.assertIn("rmp_entry_point_is_declared_exactly_once", said)


class PlainCGameTest(unittest.TestCase):
    """examples/plain_c is the way out of the framework, and the way out did
    not work: its recipe deleted src/rmp/ and CMake stopped at "No SOURCES
    given to target: rmp"; the example ignored the CI frame budget, so it ran
    until killed and was never booted; `<smoke_test.h>` was on no plain C
    game's include path; and every release archive shipped resources.rres
    alone, which raw raylib cannot read. The rmp_new job follows the recipe
    in a fresh game and runs its render check; these hold the pieces."""

    EXAMPLE = REPO / "examples" / "plain_c" / "src" / "main.c"

    def test_the_framework_library_is_built_only_when_it_is_there(self):
        text = (REPO / "CMakeLists.txt").read_text()
        guard = text.index("if(RMP_SOURCES)")
        self.assertLess(guard, text.index("add_library(rmp STATIC"))
        add_game = function_body(text.replace("endfunction()", "\n}\n"),
                                 "function(rmp_add_game NAME DIR)")
        plain = add_game[add_game.index("if(_plain_c)"):add_game.index("else()")]
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/tests"', plain)
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/include"', plain)

    def test_a_plain_c_game_reaches_admob_h_on_every_target(self):
        """rmp/ads.h says a game in plain C can call <admob.h> directly, and
        the header is written for it: the real calls on Android, static
        inline no-ops everywhere else. Android's build had thirdparty/raymob
        on the game's include path; the plain-C branch of CMakeLists.txt did
        not, so the same game stopped at "'admob.h' file not found" on every
        desktop target and the web."""
        text = (REPO / "CMakeLists.txt").read_text()
        add_game = function_body(text.replace("endfunction()", "\n}\n"),
                                 "function(rmp_add_game NAME DIR)")
        plain = add_game[add_game.index("if(_plain_c)"):add_game.index("else()")]
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/raymob"', plain)
        android = (REPO / "raymob" / "app" / "src" / "main" / "cpp" / "CMakeLists.txt").read_text()
        game_includes = android[android.index("target_include_directories(${APP_LIB_NAME}"):]
        self.assertIn('"${RAYMOB_DIR}"', game_includes[:game_includes.index(")\n")])
        self.assertTrue((REPO / "thirdparty" / "raymob" / "admob.h").is_file())

    def test_nothing_touches_the_rmp_target_where_there_may_be_none(self):
        """The clang flag for designated initialisers arrived as
        `target_compile_options(rmp PUBLIC ...)` below the library, and in a
        game with no src/rmp/ that is "Cannot specify compile options for
        target rmp which is not built by this project". Every command on the
        rmp target sits under a condition that says it exists."""
        stack = []
        found = 0
        for line in (REPO / "CMakeLists.txt").read_text().splitlines():
            code = line.split("#", 1)[0].strip()
            if re.match(r"if\s*\(", code):
                stack.append(code)
            elif re.match(r"(else|elseif)\s*\(", code) and stack:
                stack[-1] = "else of " + stack[-1]
            elif re.match(r"endif\s*\(", code) and stack:
                stack.pop()
            if re.match(r"(target_\w+|set_property\(TARGET|add_library)\s*\(?\s*rmp\b", code) \
                    or re.match(r"set_property\(TARGET rmp\b", code):
                found += 1
                with self.subTest(line=code[:60]):
                    self.assertTrue(any(c.startswith(("if(RMP_SOURCES", "if(TARGET rmp"))
                                        or "AND TARGET rmp" in c for c in stack),
                                    f"{code} runs where there may be no rmp target")
        self.assertGreater(found, 6)

    def test_the_example_runs_under_the_ci_frame_budget(self):
        code = "\n".join(line for line in self.EXAMPLE.read_text().splitlines()
                         if not line.lstrip().startswith("//"))
        for call in ("SmokeTest_Begin();", "SmokeTest_ReportBoot(", "SmokeTest_Done()",
                     "SmokeTest_CaptureFrame();", "SmokeTest_Tick();"):
            with self.subTest(call=call):
                self.assertIn(call, code)
        self.assertLess(code.index("SmokeTest_CaptureFrame();"), code.index("EndDrawing();"))
        self.assertIn("#include <smoke_test.h>", code)
        self.assertNotIn("../tests/smoke_test.h", code)
        # A boot line printed by hand cannot say no: the hooks' one checks the window.
        self.assertNotIn('"RAY_TEST_BOOT_OK', code)

    def test_its_release_moves_only_when_resources_is_next_to_it(self):
        """The framework's entry point moves into the executable's folder only
        when resources/ is there (enter_executable_folder()). The example moved
        always, so a release run from the project folder read
        build/release/resources/, which does not exist. The rmp_new job runs the shipped check on the
        plain C game, which starts it from the project folder too."""
        code = "\n".join(line for line in self.EXAMPLE.read_text().splitlines()
                         if not line.lstrip().startswith("//"))
        move = code.index("ChangeDirectory(folder);")
        guard = code[code.rindex("#if RMP_PRODUCTION_BUILD", 0, move):move]
        self.assertIn("DirectoryExists(TextFormat(\"%s%s\", folder, RMP_RESOURCES_PATH))",
                      guard)
        self.assertIn("folder[0] != '\\0'", guard)
        self.assertEqual(code.count("ChangeDirectory("), 1)
        ci = (REPO / ".github" / "workflows" / "ci.yml").read_text()
        step = ci[ci.index("- name: A game in plain C, made the way examples/plain_c says"):]
        step = step[:step.index("\n\n")]
        self.assertIn('sh tools/shipped_check.sh Ninja "" "$(python3 tools/configure.py '
                      '--print-name)"', step)

    def test_the_examples_job_boots_it(self):
        script = (REPO / "tools" / "examples_build.sh").read_text()
        self.assertNotIn('"$t" = "example_plain_c"', script)
        self.assertIn('if [ "$ran" -ne "$expected" ]', script)

    def test_the_recipe_is_where_ci_reads_it(self):
        recipe = re.findall(r"^//     (rm src/main\.cpp .*)$", self.EXAMPLE.read_text(), re.M)
        self.assertEqual(recipe, ["rm src/main.cpp && rm -r src/rmp/ src/scenes/ tests/game/"])
        ci = (REPO / ".github" / "workflows" / "ci.yml").read_text()
        self.assertIn("- name: A game in plain C, made the way examples/plain_c says", ci)
        self.assertIn('sh -c "$RECIPE"', ci)
        readme = (REPO / "README.md").read_text()
        self.assertIn(recipe[0], readme)
        self.assertNotIn("(examples/plain_c/main.c)", readme)


class IosRefusesOutOfOrderInitialisersTest(unittest.TestCase):
    """-Werror=reorder-init-list reached every CMake target and Android's
    native build, and not the third build system: the iOS project XcodeGen
    makes from ios/project.yml, where Clang only warns about
    `{ .grow_y = true, .width = 8 }` -- which GCC refuses."""

    def project(self, **settings):
        cfg = base_config()
        cfg["ios"]["settings"] = settings
        captured = {}
        original = cfgmod.write
        cfgmod.write = lambda path, content: captured.__setitem__(str(path), content)
        try:
            with quiet():
                cfgmod.gen_ios_project(cfg)
        finally:
            cfgmod.write = original
        (text,) = (v for k, v in captured.items() if k.endswith("project.yml"))
        return text

    def test_the_flag_is_in_the_ios_project(self):
        text = self.project()
        self.assertRegex(text, r'\n        OTHER_CPLUSPLUSFLAGS: "?\$\(inherited\) -Werror=reorder-init-list"?\n')

    def test_it_is_the_flag_cmake_gives_clang(self):
        cmake = (REPO / "CMakeLists.txt").read_text()
        self.assertIn(f"set(RMP_STRICT_CXX_FLAGS {cfgmod.IOS_STRICT_CXX_FLAGS})", cmake)

    def test_a_games_own_flags_are_kept_in_the_same_key(self):
        text = self.project(OTHER_CPLUSPLUSFLAGS="-DMY_GAME=1")
        self.assertEqual(text.count("OTHER_CPLUSPLUSFLAGS:"), 1)
        self.assertIn("$(inherited) -Werror=reorder-init-list -DMY_GAME=1", text)


class WarningsOnOurCodeTest(unittest.TestCase):
    """Our code compiled with no warning flag at all: the only one was
    -Werror=reorder-init-list, and -Wshadow reached it through clang-tidy.
    -Wall -Wextra go on every target built from our sources, the vendored
    headers are SYSTEM so their warnings are not reported as ours, and
    RMP_WERROR makes them errors where the framework checks itself."""

    # CMakeLists.txt and every file it includes from cmake/: game_test is
    # made in cmake/game_tests.cmake, and a target there is ours all the same.
    CMAKE = "\n".join(p.read_text() for p in
                      [REPO / "CMakeLists.txt", *sorted((REPO / "cmake").glob("*.cmake"))])

    def test_every_target_of_ours_gets_the_warnings(self):
        made = re.findall(r"^\s*add_(?:executable|library)\((\$\{\w+\}|\w+)", self.CMAKE, re.M)
        self.assertGreaterEqual(len(made), 8, made)
        for target in sorted(set(made)):
            with self.subTest(target=target):
                self.assertTrue(f"rmp_apply_warnings({target})" in self.CMAKE,
                                f"{target} is built from our sources and has no rmp_apply_warnings()")

    def test_the_flags_and_what_they_leave_out(self):
        body = self.CMAKE[self.CMAKE.index("function(rmp_apply_warnings"):]
        body = body[:body.index("endfunction()")]
        self.assertIn("-Wall -Wextra -Wno-missing-field-initializers", body)
        self.assertIn("if(RMP_WERROR)", body)
        self.assertIn("-Werror", body)
        # raylib keeps -w, and its headers reach our files as SYSTEM.
        self.assertNotIn("rmp_apply_warnings(raylib)", self.CMAKE)
        self.assertIn("set_property(TARGET raylib PROPERTY SYSTEM TRUE)", self.CMAKE)

    def test_no_vendored_directory_is_a_plain_include(self):
        """With thirdparty/ as plain -I, gcc printed 1293 warnings that were
        not ours. Every target_include_directories naming thirdparty/ is
        SYSTEM."""
        calls = re.findall(r"target_include_directories\(([^)]*)\)", self.CMAKE)
        vendored = [c for c in calls if "thirdparty" in c]
        self.assertGreaterEqual(len(vendored), 3)
        for call in vendored:
            with self.subTest(call=" ".join(call.split())[:80]):
                self.assertRegex(call, r"^\S+\s+SYSTEM\s")

    def test_the_framework_checks_itself_with_werror(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("rmp_cli_werror", REPO / "tools" / "rmp.py")
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        unit = next(s for s in cli.STAGES if s.name == "unit")
        configure = next(step for step in unit.steps if step[0] == "configure")
        self.assertIn("-DRMP_WERROR=ON", configure[1])
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("cmake --preset debug -DBUILD_TESTS=ON -DRMP_WERROR=ON", lint)
        self.assertIn("-DRMP_WERROR=ON", (REPO / "tools" / "sanitize_check.sh").read_text())

    def test_a_game_is_never_made_to(self):
        """A newer compiler on a game author's machine must not stop a build
        that was fine yesterday: the option is OFF unless asked for."""
        self.assertIn('option(RMP_WERROR "Make a compiler warning in our own code an error" OFF)',
                      self.CMAKE)


class SmallTruthsTest(unittest.TestCase):
    """Sentences that were false about the code next to them."""

    def test_assets_h_says_where_the_framework_catches(self):
        """It said the framework "has no exceptions anywhere", and
        src/rmp/save.cpp catches three times, around standard-library calls
        that can throw."""
        text = (REPO / "include" / "rmp" / "assets.h").read_text()
        self.assertNotIn("no exceptions anywhere", text)
        catches = (REPO / "src" / "rmp" / "save.cpp").read_text().count("catch (...)")
        self.assertGreater(catches, 0)
        self.assertIn("src/rmp/save.cpp", text)

    def test_smoke_test_h_names_its_note_and_not_a_line(self):
        """"the note at CMakeLists.txt:599" pointed at something else the day
        the file grew. A pointer into another file names what it points at."""
        for path in [REPO / "tests" / "smoke_test.h", *sorted((REPO / "include" / "rmp").glob("*.h")),
                     *sorted((REPO / "src" / "rmp").rglob("*.cpp"))]:
            with self.subTest(file=path.relative_to(REPO).as_posix()):
                self.assertNotRegex(path.read_text(), r"\b[\w./]+\.(txt|cpp|h|py|sh|yml):\d+\b")

    def test_no_note_names_a_tool_that_is_gone(self):
        """tools/license_check.sh became tools/license_db.py, and two notes
        still named it. (The retired runner in FROZEN_VERSIONS.md is
        DocsTest.test_no_just_is_left's, which now reads the notes in
        thirdparty/ that are ours.)"""
        for rel in ("thirdparty/FROZEN_VERSIONS.md", "thirdparty/raylib/PATCHES.md",
                    "README.md", "TECHNICAL.md"):
            text = (REPO / rel).read_text()
            for tool in re.findall(r"tools/[\w.]+\.(?:sh|py)", text):
                with self.subTest(file=rel, tool=tool):
                    self.assertTrue((REPO / tool).is_file(), f"{rel} names {tool}, which is gone")

    def test_configure_py_calls_this_a_framework(self):
        docstring = ast.get_docstring(ast.parse((REPO / "tools" / "configure.py").read_text()))
        self.assertNotIn("template", docstring.lower())

    def test_the_seam_check_says_where_the_clock_is_read(self):
        """seam_check.sh called step_delta() "the single GetFrameTime() in the
        whole framework", and the UI read it in two more places. The UI has its
        one read now, frame_time() -- the animation clock goes through it --
        and the note says there are two seams for the clock: the game's and the
        UI's."""
        style = (REPO / "src" / "rmp" / "ui" / "style.cpp").read_text()
        self.assertNotIn("GetFrameTime()", "\n".join(l.split("//")[0] for l in style.splitlines()))
        seam = (REPO / "tools" / "seam_check.sh").read_text()
        self.assertNotIn("is the single", seam)
        self.assertNotIn('"src/rmp/ui/style.cpp"', seam)

    @staticmethod
    def toml_comment(table):
        text = (REPO / "raylib_multiplatform.toml").read_text()
        body = text[text.index(f"\n[{table}]\n"):]
        body = body[:body.index("\n[", 1)]
        return re.sub(r"\s*\n#\s*", " ", body)

    def test_portable_saves_name_the_bsds_that_cannot_have_them(self):
        """[save] said a portable folder exists on "Windows, Linux and the
        BSDs". raylib's GetApplicationDirectory() answers "" on NetBSD and
        OpenBSD, and portable_folder() falls back to the user's folder there."""
        save = (REPO / "src" / "rmp" / "save.cpp").read_text()
        self.assertIn("this system does not say where the", save)
        comment = self.toml_comment("save")
        self.assertNotIn("Windows, Linux and the BSDs have", comment)
        self.assertIn("NetBSD and OpenBSD cannot say where the executable is", comment)
        generated = "\n".join(e for e in cfgmod.APP_DEFINES if isinstance(e, str))
        self.assertNotIn("Windows, Linux and the BSDs", generated)

    def test_the_icon_says_which_platforms_get_one(self):
        """[icon] said EVERY platform's icon comes out of the one file, and a
        new game's README said "the app icon on every platform".
        generate_icons() makes Android's, the iOS AppIcon and the Windows .ico;
        macOS, Linux, the BSDs and the web get none."""
        configure = (REPO / "tools" / "configure.py").read_text()
        body = configure[configure.index("def generate_icons("):]
        body = body[:body.index("\ndef ", 1)]
        for call in ("generate_ios_icon(", "generate_windows_icon(", '"raymob" / "app"'):
            self.assertIn(call, body)
        comment = self.toml_comment("icon")
        self.assertNotRegex(comment, r"(?i)every platform")
        self.assertIn("The app icon on Android, iOS and Windows", comment)
        rmp_py = (REPO / "tools" / "rmp.py").read_text()
        readme = rmp_py[rmp_py.index("def game_readme("):]
        readme = readme[:readme.index("\ndef ", 1)]
        self.assertNotIn("on every platform", readme)
        self.assertIn("the app icon on Android, iOS and Windows", readme)

    def test_the_seam_check_says_what_it_scans(self):
        """Its first line said "a file under src/rmp/" while the scan has read
        include/rmp/ too since the entry-point macros were found invisible to
        it, and it called the UI's reads "phase 5's work", which phase 5 did
        not take (the note under the list says why)."""
        seam = (REPO / "tools" / "seam_check.sh").read_text()
        scanned = re.findall(r"find (src/rmp include/rmp|src/rmp) ", seam)
        self.assertTrue(scanned)
        self.assertEqual(set(scanned), {"src/rmp include/rmp"})
        header = re.sub(r"\s*\n#\s*", " ", seam[:seam.index("\nset -uo pipefail")])
        start = header.index("Fail if")
        first = header[start:header.index(".", start)]
        self.assertIn("src/rmp/ or include/rmp/", first)
        self.assertNotIn("phase 5", header)


class ConfigureCombinationTest(unittest.TestCase):
    """Pairs of settings that are each valid and cannot both be honoured.

    These are the expensive ones. A wrong value fails loudly at the next step; a
    wrong COMBINATION builds, ships, and does the other thing you asked for — the
    rgfw/wayland pair produced an X11 binary from a config that said Wayland.
    """

    def reject(self, **overrides):
        cfg = base_config(**overrides)
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        return str(caught.exception)

    def test_rgfw_and_wayland_cannot_both_hold(self):
        message = self.reject(linux={"backend": "rgfw", "wayland": True})
        self.assertIn("X11", message)
        # And it says which one to change, because "these conflict" leaves the
        # user to guess which of the two they meant.
        self.assertIn("Pick one", message)

    def test_glfw_with_wayland_is_fine(self):
        cfg = base_config(linux={"backend": "glfw", "wayland": True})
        with quiet():
            cfgmod.validate(cfg, False)

    def test_rshapes_cannot_be_disabled_because_the_ui_draws_with_it(self):
        message = self.reject(raylib={"disabled_modules": ["rshapes"]})
        self.assertIn("rmp::ui", message)

    def test_the_modules_that_can_still_be_disabled(self):
        for mod in ("rmodels", "raudio"):
            with self.subTest(module=mod):
                cfg = base_config(raylib={"disabled_modules": [mod]})
                with quiet():
                    cfgmod.validate(cfg, False)

    def test_half_a_deploy_target_is_rejected_in_both_directions(self):
        for user, game in (("omardev", ""), ("", "my-game")):
            with self.subTest(user=user, game=game):
                message = self.reject(deploy=dict(
                    copy.deepcopy(cfgmod.DEFAULTS["deploy"]),
                    itch={"user": user, "game": game}))
                self.assertIn("go together", message)

    def test_both_empty_means_off_and_is_allowed(self):
        cfg = base_config(deploy=dict(copy.deepcopy(cfgmod.DEFAULTS["deploy"]),
                                      itch={"user": "", "game": ""}))
        with quiet():
            cfgmod.validate(cfg, False)


class ConfigureHostToolchainTest(unittest.TestCase):
    """[dev] compiler naming a toolchain this machine cannot mean.

    CMake refuses these too, and that refusal stays. This exists because the
    rule is that a config mistake never costs a configure step — and by the time
    CMake speaks, raylib is already being detected.
    """

    def check(self, compiler, system):
        cfg = base_config(dev={"compiler": compiler, "linker": "auto"})
        original = cfgmod.platform.system
        cfgmod.platform.system = lambda: system
        try:
            with quiet():
                cfgmod.validate(cfg, False)
            return None
        except cfgmod.ConfigError as exc:
            return str(exc)
        finally:
            cfgmod.platform.system = original

    def test_windows_toolchains_are_rejected_off_windows(self):
        for compiler in ("mingw", "msvc"):
            for system in ("Linux", "Darwin", "FreeBSD"):
                with self.subTest(compiler=compiler, system=system):
                    message = self.check(compiler, system)
                    self.assertIsNotNone(message, f"{compiler} accepted on {system}")
                    self.assertIn(system, message)

    def test_they_are_fine_on_windows(self):
        for compiler in ("mingw", "msvc"):
            with self.subTest(compiler=compiler):
                self.assertIsNone(self.check(compiler, "Windows"))

    def test_the_portable_ones_are_fine_everywhere(self):
        for compiler in ("clang", "gcc", "default"):
            for system in ("Linux", "Windows", "Darwin"):
                with self.subTest(compiler=compiler, system=system):
                    self.assertIsNone(self.check(compiler, system))


class ConfigureSanitizeTest(unittest.TestCase):
    """[dev] sanitize: which sanitizers a Debug build is instrumented with.

    Every refusal here is something that can never work -- a sanitizer a game
    cannot use, a compiler that has no runtime for it, a platform whose clang
    refuses the flag. A compiler that merely lacks the runtime on THIS machine
    is not refused: cmake/sanitize.cmake runs a sanitized program and warns.
    Each refusal names the `[dev] sanitize` line, says why, and says what to
    write instead."""

    def outcome(self, sanitize, compiler="clang", system="Linux"):
        cfg = base_config(dev={"compiler": compiler, "linker": "auto", "sanitize": sanitize})
        original = cfgmod.platform.system
        cfgmod.platform.system = lambda: system
        try:
            with quiet():
                cfgmod.validate(cfg, False)
            return None
        except cfgmod.ConfigError as exc:
            where = cfgmod.locate_from(exc)
            self.assertIsNotNone(where, f"{exc} cannot be located")
            self.assertRegex(where[1], r"^sanitize\s*=",
                             f"{exc} is not located at the [dev] sanitize line")
            return str(exc)
        finally:
            cfgmod.platform.system = original

    VALID = ([], ["address"], ["undefined"], ["address", "undefined"], ["undefined", "address"])

    def test_the_valid_lists_on_every_desktop(self):
        for system in ("Linux", "Darwin", "FreeBSD", "NetBSD", "Windows"):
            for sanitize in self.VALID:
                with self.subTest(system=system, sanitize=sanitize):
                    self.assertIsNone(self.outcome(sanitize, system=system))

    # name -> the words of its reason, which is what makes it more than a no.
    REFUSED = {
        "thread": "cannot be combined with address",
        "memory": "down to the C++ standard library",
        "leak": "already part of address",
        "hwaddress": "AArch64 Linux and Android only",
        "integer": "not undefined behaviour",
        "implicit-conversion": "defined behaviour",
        "fuzzer": "replaces main()",
    }

    def test_every_refused_sanitizer_says_why(self):
        self.assertEqual(set(self.REFUSED), set(cfgmod.SANITIZE_REFUSED))
        for name, why in self.REFUSED.items():
            with self.subTest(name=name):
                message = self.outcome(["address", name])
                self.assertIsNotNone(message, f"{name} accepted")
                self.assertIn("[dev] sanitize", message)
                self.assertIn(f"{name!r} is refused", message)
                self.assertIn(why, message)
                self.assertIn('Use "address" and "undefined"', message)

    def test_an_unknown_name_gets_the_nearest_one(self):
        for typo, near in (("adress", "address"), ("undefinded", "undefined"),
                           ("Address", "address"), ("threads", "thread")):
            with self.subTest(typo=typo):
                message = self.outcome([typo])
                self.assertIn(f"unknown sanitizer {typo!r}", message)
                self.assertIn(f"Did you mean {near!r}?", message)
                self.assertIn("address, undefined", message)

    def test_a_comma_list_in_one_string_is_told_to_split(self):
        """What -fsanitize= spells with commas, TOML spells as two strings."""
        message = self.outcome(["address,undefined"])
        self.assertIn("unknown sanitizer 'address,undefined'", message)
        self.assertIn("One name per string: ['address', 'undefined']", message)

    def test_nothing_near_is_not_guessed(self):
        message = self.outcome(["xyzzy"])
        self.assertIn("unknown sanitizer 'xyzzy'", message)
        self.assertNotIn("Did you mean", message)
        self.assertIn("unknown sanitizer ''", self.outcome([""]))

    def test_twice_is_a_mistake(self):
        self.assertIn("lists a sanitizer twice", self.outcome(["address", "address"]))

    def test_the_wrong_types(self):
        for value, said in (("address", "is a string, and this has to be a list"),
                            (True, "has to be a list of strings"),
                            (["address", 1], "has to be a list of strings"),
                            ({"address": True}, "has to be a list of strings")):
            with self.subTest(value=value):
                message = self.outcome(value)
                self.assertIn("[dev] sanitize", message)
                self.assertIn(said, message)

    def test_msvc_has_address_and_not_undefined(self):
        self.assertIsNone(self.outcome(["address"], compiler="msvc", system="Windows"))
        for sanitize in (["undefined"], ["address", "undefined"]):
            with self.subTest(sanitize=sanitize):
                message = self.outcome(sanitize, compiler="msvc", system="Windows")
                self.assertIn("has no UndefinedBehaviorSanitizer", message)
                self.assertIn('sanitize = ["address"]', message)

    def test_mingw_has_none(self):
        self.assertIsNone(self.outcome([], compiler="mingw", system="Windows"))
        for sanitize in (["address"], ["undefined"], ["address", "undefined"]):
            for system in ("Windows", "MINGW64_NT-10.0-26100"):
                with self.subTest(sanitize=sanitize, system=system):
                    message = self.outcome(sanitize, compiler="mingw", system=system)
                    self.assertIn("ships no sanitizer runtime", message)
                    self.assertIn("CLANG64", message)

    def test_openbsd_has_no_address(self):
        self.assertIsNone(self.outcome(["undefined"], system="OpenBSD"))
        self.assertIsNone(self.outcome([], system="OpenBSD"))
        message = self.outcome(["address", "undefined"], system="OpenBSD")
        self.assertIn("OpenBSD's clang has no AddressSanitizer", message)
        self.assertIn('sanitize = ["undefined"]', message)

    def test_it_reaches_cmake_as_a_list(self):
        for sanitize, line in ((["address", "undefined"], 'set(RMP_DEV_SANITIZE "address;undefined")'),
                               ([], 'set(RMP_DEV_SANITIZE "")')):
            captured = {}
            original = cfgmod.write
            cfgmod.write = lambda path, content: captured.__setitem__(Path(path).name, content)
            try:
                cfgmod.gen_dev_sanitize(base_config(dev={"sanitize": sanitize}))
            finally:
                cfgmod.write = original
            with self.subTest(sanitize=sanitize):
                self.assertIn(line, captured["dev_sanitize.cmake"])

    def test_gen_cmake_writes_it(self):
        captured = {}
        original = cfgmod.write
        cfgmod.write = lambda path, content: captured.__setitem__(Path(path).name, content)
        try:
            with quiet():
                cfgmod.gen_cmake(base_config())
        finally:
            cfgmod.write = original
        self.assertIn("dev_sanitize.cmake", captured)

    def test_the_schema_offers_the_two(self):
        self.assertEqual(cfgmod.ALLOWED_ITEMS["dev.sanitize"](), ["address", "undefined"])

    def test_the_toml_comment_names_what_validate_takes_and_refuses(self):
        section = toml_section("dev")
        self.assertIn("address | undefined, or [] for none", section)
        for name in cfgmod.SANITIZE_REFUSED:
            with self.subTest(refused=name):
                self.assertRegex(section, rf"#[^\n]*\b{re.escape(name)}\b")
        self.assertNotIn("CI builds `release`/`web` and is\n# unaffected", section,
                         "the render check CI runs on every push is a Debug build")



def lsan_suppressions(source: str) -> list[str]:
    """The lines __lsan_default_suppressions() returns, from the C source:
    its adjacent string literals joined, the way the compiler joins them."""
    body = source[source.index("__lsan_default_suppressions(void) {"):]
    body = body[:body.index("\n}")]
    literal = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    return [line for line in literal.encode().decode("unicode_escape").split("\n") if line]


def our_paths() -> list[str]:
    import subprocess
    tracked = subprocess.run(["git", "-c", "safe.directory=*", "ls-files"], cwd=REPO,
                             capture_output=True, text=True).stdout.split()
    return [f for f in tracked if f.startswith(("src/", "include/", "tests/", "examples/",
                                                "cmake/", "tools/"))]


def suppression_problems(lines: list[str], paths: list[str], names: list[str]) -> list[str]:
    """What is wrong with a list of LeakSanitizer suppressions: each has to
    name a shared library by its file name, and none may match a file or a
    name of ours. LSan matches the pattern, `*` a wildcard and ^/$ anchors,
    anywhere in a frame's function, file or module."""
    problems = []
    for line in lines:
        kind, _, pattern = line.partition(":")
        if kind != "leak" or not pattern:
            problems.append(f"{line!r} is not a leak:<library> line")
            continue
        if not re.fullmatch(r"\^?lib[A-Za-z0-9_+.-]*\*?", pattern) or ".so" not in pattern \
                and not pattern.endswith("*"):
            problems.append(f"{line!r} does not name a shared library (lib<name>.so)")
        literal = re.compile(".*".join(re.escape(part) for part in
                                       pattern.strip("^$").split("*")))
        for candidate in paths + names:
            if literal.search(candidate):
                problems.append(f"{line!r} matches {candidate}, which is ours")
                break
    return problems


class SanitizerSuppressionsTest(unittest.TestCase):
    """cmake/sanitizer_hooks.c carries the LeakSanitizer suppressions for the
    system's libraries, and cmake/sanitize_ignore.txt what clang's UBSan does
    not instrument. Both are decisions about code that is NOT ours, and a line
    broad enough to reach ours would silence the very reports the sanitizers
    are there for -- the leak canary in tests/sanitize_test.cpp is the proof
    that runs, this is the one that reads."""

    HOOKS = (REPO / "cmake" / "sanitizer_hooks.c").read_text()
    OUR_NAMES = ["rmp", "librmp.a", "ray_test", "unit_test", "ui_layout_test", "sanitizer_canary",
                 "platformer_play", "input_play", "example_games_07_platformer",
                 "leak_on_purpose", "_ZN3rmp"]

    def test_no_suppression_reaches_our_code(self):
        lines = lsan_suppressions(self.HOOKS)
        self.assertEqual(suppression_problems(lines, our_paths(), self.OUR_NAMES), [])

    def test_the_check_sees_each_kind_of_mistake(self):
        paths = ["src/rmp/app.cpp", "tests/sanitizer_canary.cpp"]
        for line, said in (("leak:rmp", "not name a shared library"),
                           ("leak:*", "matches"),
                           ("leak:app", "matches src/rmp/app.cpp"),
                           ("leak:librmp*", "matches librmp.a"),
                           ("fun:libX11.so", "not a leak:"),
                           ("leak:", "not a leak:")):
            with self.subTest(line=line):
                found = "\n".join(suppression_problems([line], paths, self.OUR_NAMES))
                self.assertIn(said, found)
        self.assertEqual(suppression_problems(["leak:libX11.so", "leak:libdbus-1.so*"],
                                              paths, self.OUR_NAMES), [])

    def test_the_reader_reads_the_c(self):
        source = ('const char *__lsan_default_suppressions(void) {\n'
                  '    return "leak:libX11.so\\n"   /* run A */\n'
                  '           "leak:libGLX_mesa.so\\n";\n}\n')
        self.assertEqual(lsan_suppressions(source), ["leak:libX11.so", "leak:libGLX_mesa.so"])

    def test_every_suppression_says_which_run_found_it(self):
        body = self.HOOKS[self.HOOKS.index("__lsan_default_suppressions(void) {"):]
        for line in lsan_suppressions(self.HOOKS):
            with self.subTest(line=line):
                at = body.index(line)
                self.assertRegex(body[at:body.index("\n", at)], r"/\*.+\*/",
                                 "each suppression carries the run that reported it")

    def test_the_ignorelist_names_vendored_code_only(self):
        """Clang's -fsanitize-ignorelist: a src: line must be under thirdparty/,
        and a fun: line must not match a function we define."""
        text = (REPO / "cmake" / "sanitize_ignore.txt").read_text()
        entries = [line.strip() for line in text.splitlines()
                   if line.strip() and not line.startswith("#")]
        self.assertGreater(len(entries), 0)
        ours = "\n".join((REPO / p).read_text(errors="replace") for p in our_paths()
                         if p.endswith((".c", ".cpp", ".h")) and p.startswith(("src/", "include/")))
        for entry in entries:
            kind, _, pattern = entry.partition(":")
            with self.subTest(entry=entry):
                self.assertIn(kind, ("fun", "src"))
                if kind == "src":
                    self.assertIn("thirdparty/", pattern)
                    continue
                literal = pattern.strip("*")
                self.assertTrue(literal)
                defined = re.findall(rf"\b\w*{re.escape(literal)}\w*\s*\([^;{{}}]*\)\s*\{{", ours)
                self.assertEqual(defined, [], f"{entry} matches a function of ours")

    def test_it_reaches_the_build_from_cmake_and_not_from_tools(self):
        """It sat under tools/, which rmp new does not copy, so a game built
        with clang's UBSan had no ignorelist and stopped in cute_tiled."""
        self.assertFalse((REPO / "tools" / "sanitize_ignore.txt").exists())
        sanitize = (REPO / "cmake" / "sanitize.cmake").read_text()
        self.assertIn('"${CMAKE_CURRENT_LIST_DIR}/sanitize_ignore.txt"', sanitize)
        self.assertIn("-fsanitize-ignorelist=${RMP_SANITIZE_IGNORELIST}", sanitize)


class FrameworkDebugIsSanitizedTest(unittest.TestCase):
    """The framework's Debug builds ALWAYS run under ASan and UBSan, and
    "always" is a set of things that can each be undone by an edit nobody
    notices: the .toml losing a name, the include moving out of its branch,
    the per-target call going missing, the canary no longer built. Each is
    held here."""

    CMAKE = (REPO / "CMakeLists.txt").read_text()
    SANITIZE = (REPO / "cmake" / "sanitize.cmake").read_text()

    def test_the_frameworks_toml_lists_both(self):
        import tomllib
        cfg = tomllib.loads((REPO / "raylib_multiplatform.toml").read_text())
        self.assertEqual(sorted(cfg["dev"]["sanitize"]), ["address", "undefined"])

    def test_cmake_reads_it_in_development_builds_only(self):
        block = self.CMAKE[self.CMAKE.index("if(NOT PRODUCTION_BUILD)\n  include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/dev_linker.cmake\""):]
        block = block[:block.index("endif()")]
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/dev_sanitize.cmake" OPTIONAL)', block)
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sanitize.cmake")', block)
        self.assertEqual(self.CMAKE.count("cmake/sanitize.cmake\")"), 1)

    def test_every_debug_target_goes_through_the_checks(self):
        body = self.CMAKE[self.CMAKE.index("function(rmp_apply_compile_flags target)"):]
        body = body[:body.index("endfunction()")]
        debug = body[body.index("  else()\n    if(MSVC)"):]
        self.assertIn("rmp_apply_debug_checks(${target})", debug)
        self.assertNotIn("rmp_apply_debug_checks", body[:body.index("  else()\n    if(MSVC)")])

    def test_the_probe_runs_a_program_and_ci_can_require_it(self):
        self.assertIn("try_run(", self.SANITIZE)
        self.assertIn('"$ENV{RMP_REQUIRE_SANITIZERS}" STREQUAL "1"', self.SANITIZE)
        self.assertIn("message(FATAL_ERROR", self.SANITIZE)
        self.assertIn("message(WARNING", self.SANITIZE)
        self.assertIn('option(RMP_SANITIZE "', self.SANITIZE)

    def test_the_hardening_is_in_every_debug_build(self):
        for flag in ("_GLIBCXX_ASSERTIONS",
                     "_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE",
                     "-ftrivial-auto-var-init=pattern"):
            with self.subTest(flag=flag):
                self.assertIn(flag, self.SANITIZE)

    def test_the_canary_is_built_before_the_tests_that_read_it(self):
        self.assertIn("add_executable(sanitizer_canary", self.CMAKE)
        self.assertIn("add_dependencies(unit_test sanitizer_canary)", self.CMAKE)
        self.assertIn('RMP_SANITIZER_CANARY="$<TARGET_FILE:sanitizer_canary>"', self.CMAKE)
        self.assertTrue((REPO / "tests" / "sanitize_test.cpp").is_file())

    def test_the_host_tool_is_left_alone(self):
        """rmp pack runs rres_pack: it must not stop a pack over a leak in a
        process that is about to exit."""
        self.assertNotIn("rmp_apply_compile_flags(rres_pack)", self.CMAKE)


class ConfigureWebBackendTest(unittest.TestCase):
    """[web] backend — which of raylib 6.0's three web paths gets built.

    The reason this is worth testing rather than trusting: all three produce a
    .html, a .js and a .wasm of roughly the same size and all three boot, so a
    silent fallback to GLFW is invisible in the artefacts. The workflow
    .github/workflows/web-backends.yml checks the other half — that the choice
    reaches raylib's compile line — and these check that the choice is
    understood here first.
    """

    def web(self, **overrides):
        return base_config(web=dict(copy.deepcopy(cfgmod.DEFAULTS["web"]), **overrides))

    def test_the_backends_that_work_are_accepted(self):
        for backend in ("glfw", "emscripten"):
            with self.subTest(backend=backend), quiet():
                cfgmod.validate(self.web(backend=backend), False)

    def test_the_default_is_glfw(self):
        """Not emscripten, and deliberately: rcore_web_emscripten.c still has
        its key mapping and drop-files unfinished. See the .toml comment."""
        self.assertEqual(cfgmod.DEFAULTS["web"]["backend"], "glfw")

    def test_an_unknown_backend_is_rejected_and_says_the_options(self):
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(self.web(backend="webgpu"), False)
        message = str(caught.exception)
        for backend in ("glfw", "emscripten"):
            self.assertIn(backend, message)

    def test_the_error_says_what_the_choice_is_not(self):
        """The one confusion this option has: Emscripten compiles the code in
        all three cases, so 'emscripten' cannot mean 'use Emscripten'."""
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(self.web(backend="wasm"), False)
        self.assertIn("compiles it either way", str(caught.exception))

    def test_a_desktop_backend_name_is_not_quietly_accepted(self):
        """win32 is a real backend name on another platform, which is exactly
        the kind of value that gets pasted into the wrong section."""
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(self.web(backend="win32"), False)

    def test_rgfw_is_refused_with_the_reason_and_not_as_unknown(self):
        """It is a real raylib backend and it compiles, so "unknown backend"
        would send somebody to check their spelling. It dies in the browser
        because it needs ASYNCIFY, which this framework does not have."""
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(self.web(backend="rgfw"), False)
        message = str(caught.exception)
        self.assertIn("ASYNCIFY", message)
        self.assertIn("emscripten_sleep", message)

    def test_it_reaches_the_generated_cmake(self):
        """The whole path, not just the check: a backend nobody writes out is a
        backend the build never hears about."""
        for backend in ("glfw", "emscripten"):
            with self.subTest(backend=backend):
                cfg = self.web(backend=backend)
                self.assertEqual(cfg["web"]["backend"], backend)

    def test_the_backend_is_not_a_number_or_a_bool(self):
        for bad in (True, 3, None, ["glfw"]):
            with self.subTest(value=bad):
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    cfgmod.validate(self.web(backend=bad), False)


class ConfigureMembershipTest(unittest.TestCase):
    """Every "must be one of" option, against values TOML can really produce.

    This class exists because of one line in another test. Trying `["glfw"]` for
    a backend — not because anything suggested it, but because an array is a
    thing a TOML file can contain — turned up a Python traceback instead of a
    config error, and the same hole was in all five membership checks: `x not in
    some_set` raises TypeError on an unhashable value.

    So the shape here is deliberate: one table of every such option, and the
    same hostile values against all of them. A check that only ever sees
    plausible input is a check that has not been tested.
    """

    # (section, key, a valid value) for every option validated by membership.
    OPTIONS = [
        ("windows", "backend", "glfw"),
        ("linux", "backend", "glfw"),
        ("web", "backend", "glfw"),
        ("dev", "compiler", "clang"),
        ("ui", "theme", "dark"),
        # These three were not here, and each crashed on a list until
        # EveryOptionRefusesTheWrongTypeTest walked DEFAULTS instead of a list.
        ("window", "orientation", "landscape"),
        ("android", "gl_version", "ES30"),
        ("dev", "linker", "auto"),
    ]

    # Everything TOML can hand over that is not a string. The unhashable ones
    # are the reason this class exists; the rest are here because "not a string"
    # should mean the same thing for all of them.
    HOSTILE = [["glfw"], {"name": "glfw"}, 3, 3.5, True, False]

    def with_value(self, section, key, value):
        base = copy.deepcopy(cfgmod.DEFAULTS[section])
        base[key] = value
        return base_config(**{section: base})

    def test_no_membership_check_can_be_crashed(self):
        for section, key, _ in self.OPTIONS:
            for value in self.HOSTILE:
                with self.subTest(option=f"[{section}] {key}", value=value):
                    cfg = self.with_value(section, key, value)
                    # A ConfigError is the pass. A TypeError is the bug this
                    # class was written for, and assertRaises(ConfigError) is
                    # what tells the two apart.
                    with self.assertRaises(cfgmod.ConfigError), quiet():
                        cfgmod.validate(cfg, False)

    def test_an_empty_string_is_rejected_rather_than_defaulted(self):
        """`backend = ""` looks like "unset" and is not. Falling back to a
        default here would mean the .toml says one thing and the build does
        another, quietly."""
        for section, key, _ in self.OPTIONS:
            with self.subTest(option=f"[{section}] {key}"):
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    cfgmod.validate(self.with_value(section, key, ""), False)

    def test_case_matters(self):
        """"GLFW" is not glfw. Accepting it would mean the set of valid values
        is bigger than the documented one, and the docs would be wrong."""
        for section, key, good in self.OPTIONS:
            with self.subTest(option=f"[{section}] {key}"):
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    # swapcase, not upper: "ES30" is already upper case.
                    cfgmod.validate(self.with_value(section, key, good.swapcase()), False)

    def test_whitespace_is_not_trimmed_into_validity(self):
        for section, key, good in self.OPTIONS:
            with self.subTest(option=f"[{section}] {key}"):
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    cfgmod.validate(self.with_value(section, key, f" {good} "), False)

    def test_every_rejection_lists_the_valid_options(self):
        """The error has to carry the answer. "backend must be one of" with no
        list is a message that sends somebody to the documentation."""
        for section, key, _ in self.OPTIONS:
            with self.subTest(option=f"[{section}] {key}"):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.with_value(section, key, "nonsense"), False)
                self.assertIn("one of", str(caught.exception))
                # And it points at a line, so "where" is answered too.
                self.assertIsNotNone(cfgmod.locate_from(caught.exception))

    def test_the_good_values_are_all_still_good(self):
        """The other half: a check that rejects everything also passes the tests
        above. These are the values the .toml documents."""
        cases = [
            ("windows", "backend", cfgmod.WINDOWS_BACKENDS),
            ("linux", "backend", cfgmod.LINUX_BACKENDS),
            ("web", "backend", cfgmod.WEB_BACKENDS),
            ("ui", "theme", cfgmod.UI_THEMES),
        ]
        for section, key, allowed in cases:
            for value in allowed:
                with self.subTest(option=f"[{section}] {key}", value=value):
                    cfg = self.with_value(section, key, value)
                    # linux rgfw needs wayland off — that pair has its own test.
                    if section == "linux" and value == "rgfw":
                        cfg["linux"]["wayland"] = False
                    with quiet():
                        cfgmod.validate(cfg, False)




class ConfigurePlatformTest(unittest.TestCase):
    """Which machine is this, and what can actually run on it.

    Both of these were bugs found on a real Windows box, in MSYS2, and both had
    the same shape: a check that knew one spelling of an answer and treated
    everything else as the opposite.
    """

    @contextlib.contextmanager
    def system(self, name):
        original = cfgmod.platform.system
        cfgmod.platform.system = lambda: name
        try:
            yield
        finally:
            cfgmod.platform.system = original

    # The strings platform.system() really returns. The MSYS2 ones are why this
    # class exists: they are Windows, and only the first entry looks like it.
    WINDOWS_SPELLINGS = ["Windows", "MSYS_NT-10.0-26100", "MINGW64_NT-10.0-22631",
                         "MINGW32_NT-6.2", "CYGWIN_NT-10.0"]
    OTHERS = ["Linux", "Darwin", "FreeBSD", "OpenBSD", "NetBSD"]

    def test_every_windows_spelling_counts_as_windows(self):
        for name in self.WINDOWS_SPELLINGS:
            with self.subTest(system=name), self.system(name):
                self.assertTrue(cfgmod.on_windows(), f"{name} should be Windows")

    def test_and_nothing_else_does(self):
        for name in self.OTHERS:
            with self.subTest(system=name), self.system(name):
                self.assertFalse(cfgmod.on_windows())

    def test_mingw_is_accepted_in_every_windows_shell(self):
        """The actual bug: [dev] compiler = "mingw" refused ON WINDOWS, in the
        shell that ships x86_64-w64-mingw32-gcc, with the message that mingw
        only makes sense on Windows."""
        for name in self.WINDOWS_SPELLINGS:
            for compiler in ("mingw", "msvc"):
                with self.subTest(system=name, compiler=compiler), self.system(name), quiet():
                    cfgmod.validate(base_config(dev={"compiler": compiler, "linker": "auto"}),
                                    False)

    def test_and_still_refused_everywhere_else(self):
        for name in self.OTHERS:
            for compiler in ("mingw", "msvc"):
                with self.subTest(system=name, compiler=compiler), self.system(name):
                    with self.assertRaises(cfgmod.ConfigError), quiet():
                        cfgmod.validate(base_config(dev={"compiler": compiler,
                                                         "linker": "auto"}), False)

    def test_mold_is_refused_where_it_cannot_emit_a_binary(self):
        """mold links ELF and nothing else — `mold --help` prints its own list
        and there is no PE/COFF or Mach-O in it. Asking for it on Windows used
        to get as far as the linker, which failed with a message about `ld` that
        did not contain the word mold."""
        for name in self.WINDOWS_SPELLINGS + ["Darwin"]:
            with self.subTest(system=name), self.system(name):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(base_config(dev={"compiler": "default",
                                                     "linker": "mold"}), False)
                self.assertIn("ELF", str(caught.exception))
                # And it says what to use instead, because "cannot work" without
                # an alternative is half an error message.
                self.assertIn("lld", str(caught.exception))

    def test_mold_is_fine_where_ELF_is(self):
        for name in ["Linux", "FreeBSD", "OpenBSD", "NetBSD"]:
            with self.subTest(system=name), self.system(name), quiet():
                cfgmod.validate(base_config(dev={"compiler": "default", "linker": "mold"}),
                                False)

    def test_auto_and_lld_are_accepted_everywhere(self):
        """auto especially: it is a speed setting, and a speed setting that can
        refuse a config is worse than a slow build. Which one it ends up using
        is decided by the build, after checking that it links."""
        for name in self.WINDOWS_SPELLINGS + self.OTHERS:
            for linker in ("auto", "lld", "default"):
                with self.subTest(system=name, linker=linker), self.system(name), quiet():
                    cfgmod.validate(base_config(dev={"compiler": "default",
                                                     "linker": linker}), False)


class ConfigurePlatformValuesTest(unittest.TestCase):
    """Every PLATFORM our build can ask for is one raylib will accept.

    raylib validates PLATFORM against a fixed list in CMakeOptions.txt and
    FATAL_ERRORs on anything else. We patch that list, because raylib 6.0 ships
    two backends it never added to it — and the failure mode is nasty: the
    .toml option exists, configure.py accepts it, and CMake refuses with
    "Unknown value" from inside the vendored tree.

    That happened to [web] backend = "emscripten" the first time the workflow
    ran. It was ALSO true of [windows] backend = "win32" and nobody had noticed,
    because the .toml has been set to rgfw — so this reads the values out of our
    own CMakeLists instead of a list somebody has to remember to update.
    """

    def platform_values_we_can_set(self):
        text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        return sorted(set(re.findall(r'set\(PLATFORM\s+"([^"]+)"', text)))

    def platform_values_raylib_accepts(self):
        options = REPO / "thirdparty" / "raylib" / "CMakeOptions.txt"
        text = options.read_text(encoding="utf-8")
        match = re.search(r'enum_option\(PLATFORM\s+"([^"]+)"', text)
        self.assertIsNotNone(match, "no enum_option(PLATFORM ...) in CMakeOptions.txt")
        return [value.strip() for value in match.group(1).split(";")]

    def test_our_cmake_only_asks_for_platforms_raylib_knows(self):
        accepted = self.platform_values_raylib_accepts()
        ours = self.platform_values_we_can_set()
        self.assertTrue(ours, "found no set(PLATFORM ...) at all — did CMakeLists change shape?")
        for value in ours:
            with self.subTest(platform=value):
                self.assertIn(
                    value, accepted,
                    f"CMakeLists.txt can set PLATFORM={value}, and raylib's "
                    f"CMakeOptions.txt does not list it. That is a FATAL_ERROR from "
                    f"inside the vendored tree, and the .toml option that leads to it "
                    f"looks perfectly valid until somebody picks it.")

    def test_the_backends_the_toml_offers_all_reach_a_known_platform(self):
        """The other direction: every backend the config accepts has to end up
        as one of the values above. A backend with no PLATFORM behind it would
        build the default and say nothing."""
        accepted = self.platform_values_raylib_accepts()
        # glfw is the default on all three platforms and sets no PLATFORM at
        # all, which is why it is not in this table.
        expected = {
            ("windows", "win32"): "Win32",
            ("windows", "rgfw"): "RGFW",
            ("linux", "rgfw"): "RGFW",
            ("web", "emscripten"): "WebEmscripten",
        }
        for (section, backend), platform in expected.items():
            with self.subTest(option=f"[{section}] backend = {backend}"):
                self.assertIn(platform, accepted)
        # And nothing in the sets is missing from the table except glfw.
        for section, allowed in (("windows", cfgmod.WINDOWS_BACKENDS),
                                 ("linux", cfgmod.LINUX_BACKENDS),
                                 ("web", cfgmod.WEB_BACKENDS)):
            for backend in allowed:
                if backend == "glfw":
                    continue
                with self.subTest(option=f"[{section}] backend = {backend}"):
                    self.assertIn((section, backend), expected,
                                  f"[{section}] backend = {backend} is accepted by "
                                  f"configure.py but this test does not know which "
                                  f"PLATFORM it produces — which means nobody does.")


class ConfigureGlibcFloorTest(unittest.TestCase):
    """[linux] glibc — the oldest glibc the shipped binary may need.

    The bug this prevents is invisible from the machine that builds: a binary
    compiled on Ubuntu 24.04 asks for glibc 2.39, which Debian 12, Ubuntu 22.04,
    RHEL 8 and Steam's sniper runtime do not have. It compiles, it links, it runs
    on the build machine, and elsewhere it dies with "version GLIBC_2.39 not
    found" and no other output.
    """

    def linux(self, **overrides):
        return base_config(linux=dict(copy.deepcopy(cfgmod.DEFAULTS["linux"]), **overrides))

    def test_the_default_clears_the_oldest_supported_distribution(self):
        """2.28 is RHEL 8, supported to 2029, and comfortably under Steam
        sniper's 2.31. Lower buys only distributions nobody is patching."""
        self.assertEqual(cfgmod.DEFAULTS["linux"]["glibc"], "2.28")

    def test_the_versions_that_matter_are_accepted(self):
        for version in ("2.17", "2.28", "2.31", "2.35", "2.39"):
            with self.subTest(glibc=version), quiet():
                cfgmod.validate(self.linux(glibc=version), False)

    def test_empty_means_build_against_this_machine(self):
        """Not an error: it is what you want locally, and tools/glibc_check.sh
        skips rather than inventing a floor nobody asked for."""
        with quiet():
            cfgmod.validate(self.linux(glibc=""), False)

    def test_nonsense_is_rejected_and_names_the_useful_versions(self):
        for bad in ("2.5", "3.1", "abc", "2", "2.28.1", "latest"):
            with self.subTest(glibc=bad):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.linux(glibc=bad), False)
                message = str(caught.exception)
                self.assertIn("2.17", message)
                self.assertIn("sniper", message)

    def test_it_has_to_be_a_string(self):
        """`glibc = 2.28` in the TOML is a float, and a float that rounds to a
        version number is exactly the typo that would sail through."""
        for bad in (2.28, 228, True, None, ["2.28"]):
            with self.subTest(glibc=bad):
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    cfgmod.validate(self.linux(glibc=bad), False)

    @staticmethod
    def linux_jobs():
        """job name -> its text, out of _linux.yml, split at the top-level keys."""
        text = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        jobs_at = text.index("\njobs:\n")
        parts = re.split(r"\n  ([a-z0-9-]+):\n", text[jobs_at:])
        return dict(zip(parts[1::2], parts[2::2]))

    def test_every_glibc_job_checks_or_reports_its_floor(self):
        """linux-riscv64-glibc was built with the distribution's riscv64 GNU
        toolchain against Ubuntu 24.04's glibc, never through
        tools/linux_build.sh, and nothing looked at the floor it ended up with
        -- while the .toml said the floor was every Linux binary's. Each glibc
        job now gates a floor -- [linux] glibc's, or riscv64's own -- or prints
        the one it has."""
        jobs = self.linux_jobs()
        gated = {"x64", "arm64"}
        own_floor = {"riscv64"}
        reported = {"drm-x64", "drm-arm64"}
        for job in gated | own_floor | reported:
            with self.subTest(job=job):
                self.assertIn(job, jobs)
                body = jobs[job]
                self.assertIn("tools/glibc_check.sh", body)
                if job in gated:
                    self.assertIn("tools/linux_build.sh", body)
                    self.assertNotRegex(body, r"glibc_check\.sh [^\n]* report")
                elif job in own_floor:
                    self.assertRegex(body, r"glibc_check\.sh [^\n]* \d+\.\d+\n")
                else:
                    self.assertRegex(body, r"glibc_check\.sh [^\n]* report")

    def test_the_toml_says_the_floor_riscv64_is_held_to(self):
        """linux-riscv64-glibc is cross-compiled against Ubuntu 24.04's riscv64
        glibc, so [linux] glibc does not reach it, and the .toml said only that
        its floor is "the build machine's glibc" -- which is 2.39, while the
        binary needs 2.38 (__isoc23_strtol). The job holds the binary to a
        number, and that number is the one the .toml's note gives."""
        body = self.linux_jobs()["riscv64"]
        held = re.search(r"glibc_check\.sh build/release/\$\{\{ inputs\.project_name \}\} "
                         r"(\d+\.\d+)\n", body)
        self.assertIsNotNone(held, "the riscv64 job does not hold the binary to a floor")
        text = (REPO / "raylib_multiplatform.toml").read_text()
        linux = text[text.index("\n[linux]"):]
        note = linux[linux.index("# glibc"):linux.index("\nglibc ")]
        sentence = re.sub(r"\s*\n#\s*", " ", note)
        self.assertRegex(sentence, rf"linux-riscv64-glibc is [^;]*\bneeds glibc "
                                   rf"{re.escape(held[1])} or newer")

    def test_the_toml_says_which_targets_the_floor_reaches(self):
        text = (REPO / "raylib_multiplatform.toml").read_text()
        linux = text[text.index("\n[linux]"):]
        linux = linux[:linux.index("\n[", 1)]
        note = linux[linux.index("# glibc"):]
        self.assertIn("linux-x64-glibc and linux-arm64-glibc", note)
        for unreached in ("linux-riscv64-glibc", "DRM"):
            with self.subTest(target=unreached):
                self.assertIn(unreached, note)

    def test_the_floor_reaches_the_tools_that_use_it(self):
        """--print-glibc is what tools/linux_build.sh and glibc_check.sh read.
        A floor nothing can see is a floor that does nothing."""
        self.assertIn("--print-glibc", (REPO / "tools" / "configure.py").read_text())
        for tool in ("linux_build.sh", "glibc_check.sh"):
            with self.subTest(tool=tool):
                self.assertIn("--print-glibc", (REPO / "tools" / tool).read_text())


def run_cli(*argv):
    """`configure.py` as CI actually calls it: argv in, stdout out.

    Returns (stdout, exception-or-None) rather than raising, because half of
    these tests are about the message a wrong value produces and the other half
    about the value a right one prints.
    """
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            cfgmod.main(list(argv))
    except cfgmod.ConfigError as exc:
        return out.getvalue(), exc
    return out.getvalue(), None


class ConfigureOnlyFilterTest(unittest.TestCase):
    """`--only`, which narrows what CI builds from a workflow input.

    It exists so one family can be tried on its own -- `-f targets=drm` instead
    of twenty minutes of matrix to watch three minutes of DRM. It is also the
    first thing in this file whose value arrives from a text box on a web page,
    so every way it can be wrong is a named error and every one of them is here.
    """

    def enabled(self):
        out, exc = run_cli("--print-targets")
        self.assertIsNone(exc)
        return json.loads(out)

    def test_a_group_narrows_to_exactly_that_group(self):
        out, exc = run_cli("--print-targets", "--only", "drm")
        self.assertIsNone(exc)
        self.assertEqual(json.loads(out), cfgmod.GROUPS["drm"])

    def test_every_group_narrows_to_a_subset_of_what_is_enabled(self):
        """The property that matters: --only filters, it never adds. A workflow
        input must not be able to build something [targets] switched off."""
        enabled = set(self.enabled())
        for group in sorted(cfgmod.GROUPS):
            with self.subTest(group=group):
                out, exc = run_cli("--print-targets", "--only", group)
                if exc is not None:          # a group with nothing enabled
                    self.assertIn("leaves nothing to build", str(exc))
                    continue
                self.assertTrue(set(json.loads(out)) <= enabled)

    def test_every_single_target_can_be_asked_for_by_name(self):
        for target in self.enabled():
            with self.subTest(target=target):
                out, exc = run_cli("--print-targets", "--only", target)
                self.assertIsNone(exc)
                self.assertEqual(json.loads(out), [target])

    def test_the_order_is_the_canonical_one_not_the_order_asked_for(self):
        out, _ = run_cli("--print-targets", "--only", "linux")
        got = json.loads(out)
        self.assertEqual(got, sorted(got, key=list(cfgmod.TARGETS).index))

    def test_an_unknown_name_names_every_name_that_would_have_worked(self):
        for bad in ("nope", "linux-x64", "DRM", "Drm", "drm ", " drm", "x64", "all "):
            with self.subTest(only=bad):
                _, exc = run_cli("--print-targets", "--only", bad)
                if bad.strip() in cfgmod.GROUPS or bad.strip() in cfgmod.TARGETS:
                    self.assertIsNone(exc, "surrounding spaces should be tolerated")
                    continue
                self.assertIsNotNone(exc, f"{bad!r} was accepted")
                self.assertIn("is not a target or a group", str(exc))
                self.assertIn("drm", str(exc))          # the list is in the message

    def test_an_empty_value_is_rejected_and_not_read_as_everything(self):
        """A dispatch input left blank arrives as "". Silently meaning `all`
        would be the wrong default: the caller asked for something."""
        for blank in ("", "   ", "\t"):
            with self.subTest(only=repr(blank)):
                _, exc = run_cli("--print-targets", "--only", blank)
                self.assertIsNotNone(exc)
                self.assertIn("empty", str(exc))

    def test_it_refuses_to_run_without_something_to_print(self):
        """--only on its own would silently narrow generation instead, which is
        not what anybody typing it means."""
        _, exc = run_cli("--only", "drm")
        self.assertIsNotNone(exc)
        self.assertIn("--print-targets", str(exc))

    def test_it_narrows_the_families_too_or_the_matrix_disagrees_with_itself(self):
        """ci.yml gates its jobs on families. If --only narrowed the targets and
        not the families, a drm-only run would still start the bsd leg."""
        out, exc = run_cli("--print-families", "--only", "drm")
        self.assertIsNone(exc)
        # `linux`, because that is the workflow that builds DRM -- and only
        # `linux`, which is the assertion that matters: the bsd, apple, windows,
        # web and android callers all have to sit this run out.
        self.assertEqual(json.loads(out), ["linux"])

    def test_it_narrows_the_bsd_matrix(self):
        out, exc = run_cli("--print-matrix", "bsd", "--only", "drm")
        self.assertIsNone(exc)
        self.assertEqual(json.loads(out).get("include"), [])

    def test_it_narrows_the_upx_list(self):
        out, exc = run_cli("--print-upx", "--only", "drm")
        self.assertIsNone(exc)
        self.assertTrue(set(json.loads(out)) <= {"linux-x64-glibc-drm"})

    def test_leaving_it_out_changes_nothing(self):
        plain, _ = run_cli("--print-targets")
        allof, _ = run_cli("--print-targets", "--only", "all")
        self.assertEqual(json.loads(plain), json.loads(allof))


class ConfigureDrmTargetTest(unittest.TestCase):
    """DRM/KMS is a shipped target now, not a thing you could opt into.

    It used to be held out of `all` because it needs /dev/dri and CI has no
    screen. That was a reason not to run it, not a reason not to build it, and
    the runners can run it now on a vkms virtual display.
    """

    def test_it_is_in_all(self):
        self.assertIn("linux-x64-glibc-drm", cfgmod.GROUPS["all"])

    def test_all_means_all(self):
        """`all` minus something is a thing to be argued for at the time, in a
        comment. Right now there is nothing held back."""
        self.assertEqual(sorted(cfgmod.GROUPS["all"]), sorted(cfgmod.TARGETS))

    def test_it_is_in_the_linux_group(self):
        self.assertIn("linux-x64-glibc-drm", cfgmod.GROUPS["linux"])

    def test_its_family_is_the_workflow_that_builds_it(self):
        """A family names the reusable workflow, and _linux.yml is what builds
        DRM. It was "drm" for one commit, which made `--only drm` resolve to a
        family no caller in ci.yml gates on: the run skipped every single job
        and finished green having built nothing."""
        self.assertEqual(cfgmod.TARGETS["linux-x64-glibc-drm"][0], "linux")

    def test_every_family_is_one_a_caller_in_ci_actually_gates_on(self):
        """The failure this catches is silent by construction: a family nobody
        calls does not error, it just quietly builds nothing."""
        ci = (REPO / ".github" / "workflows" / "ci.yml").read_text()
        for family in sorted({f for f, _ in cfgmod.TARGETS.values()}):
            with self.subTest(family=family):
                self.assertIn(f"outputs.families), '{family}'", ci,
                              f"no job in ci.yml is gated on the {family!r} family")

    def test_every_linux_target_follows_os_arch_libc_env(self):
        """The naming rule, enforced instead of remembered. `env` is the
        windowing system and is only there when it can differ -- absent means
        the binary carries every backend and picks at startup.
        """
        arches = {"x64", "arm64", "riscv64"}
        libcs = {"glibc", "musl"}
        envs = {"drm", "wayland", "x11"}
        for target in cfgmod.TARGETS:
            if not target.startswith("linux-"):
                continue
            with self.subTest(target=target):
                parts = target.split("-")
                self.assertIn(len(parts), (3, 4), "linux-<arch>-<libc>[-<env>]")
                self.assertEqual(parts[0], "linux")
                self.assertIn(parts[1], arches, "second segment is the architecture")
                self.assertIn(parts[2], libcs, "third segment is the C library")
                if len(parts) == 4:
                    self.assertIn(parts[3], envs, "fourth segment is the windowing system")

    def test_the_job_names_follow_the_targets(self):
        """A job called `musl` when the target is linux-x64-musl is a name that
        has to be translated every time somebody reads a red run."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        for job in ("x64:", "arm64:", "musl-x64:", "riscv64:", "drm-x64:", "drm-arm64:"):
            with self.subTest(job=job):
                self.assertIn(f"\n  {job}", workflow)

    def test_every_linux_target_has_a_job_that_names_it(self):
        """The failure this catches builds nothing and says nothing: a target in
        the .toml that no job is gated on simply never gets built, and the run
        goes green because every job it had was skipped. That happened once,
        through the family, and this is the same shape one level down."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        for target in cfgmod.TARGETS:
            if not target.startswith("linux-"):
                continue
            with self.subTest(target=target):
                self.assertIn("'" + target + "'", workflow,
                              "no job in _linux.yml is gated on " + target)

    def test_the_arm64_drm_job_runs_on_an_arm64_machine(self):
        """A native build, not a cross-compile: zig cannot link this target at
        all (see the next test), so the only way to get an aarch64 DRM binary is
        an aarch64 runner. On ubuntu-24.04 it would quietly make an x86-64 one,
        which is why the ELF machine byte is checked in the job."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        block = re.split(r"\n  \S", workflow.split("\n  drm-arm64:", 1)[1], maxsplit=1)[0]
        self.assertIn("runs-on: ubuntu-24.04-arm", block)
        self.assertIn("b700", block)          # e_machine for aarch64

    def test_drm_cannot_honour_the_glibc_floor_and_says_so_instead(self):
        """Both DRM jobs report their real floor rather than gating on one they
        cannot meet. libdrm, libgbm and libEGL come from the distribution and
        are built against ITS glibc, so linking them against an older stub set
        fails on the .so and not on anything we wrote."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        for job in ("drm-x64", "drm-arm64"):
            with self.subTest(job=job):
                block = re.split(r"\n  \S", workflow.split("\n  " + job + ":", 1)[1],
                                 maxsplit=1)[0]
                # The invocation, not the word. Written as assertIn("report",
                # block) this passed with the step deleted, because the comment
                # above it says "`report`, not a gate" -- a test that green-lit
                # its own absence, caught by running it against the deletion.
                self.assertRegex(block, r"glibc_check\.sh[^\n]* report\b")
        check = (REPO / "tools" / "glibc_check.sh").read_text()
        self.assertIn('[ "$WANT" = "report" ]', check)
        self.assertIn("libdrm", check)

    def test_the_drm_job_is_not_skipped_when_the_target_is_enabled(self):
        """It was `if: contains(...)` with no `full`, so it never ran on the
        fast lane and, once drm entered `all`, would have run on every push."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        block = re.split(r"\n  \S", workflow.split("\n  drm-x64:", 1)[1], maxsplit=1)[0]
        self.assertIn("inputs.full", block)
        self.assertIn("linux-x64-glibc-drm", block)

    def test_the_drm_binary_is_actually_run_and_not_just_linked(self):
        """A job that only checks `ldd` proves the build, not the backend. DRM
        has no windowing system for xvfb to fake, so the kernel supplies a
        virtual one: vkms plus Mesa's kms_swrast."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        self.assertIn("\n  drm-x64-run:", workflow)
        block = re.split(r"\n  \S", workflow.split("\n  drm-x64-run:", 1)[1], maxsplit=1)[0]
        self.assertIn("modprobe vkms", block)
        self.assertIn("--device /dev/dri", block)
        for marker in ("RAY_TEST_BOOT_OK", "RAY_TEST_RENDER_OK", "RAY_TEST_DONE_FRAMES"):
            with self.subTest(marker=marker):
                self.assertIn(marker, block)

    def test_the_drm_run_job_does_not_nest_a_mount_under_a_read_only_one(self):
        """docker cannot create a mountpoint inside a read-only rootfs and
        podman can, so the nested form passes every local run and dies on the
        first runner. It cost a round once already."""
        workflow = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        for job in ("drm-x64-run", "musl-x64-run"):
            with self.subTest(job=job):
                block = re.split(r"\n  \S", workflow.split(f"\n  {job}:", 1)[1], maxsplit=1)[0]
                mounts = re.findall(r"-v \"\$PWD/([^:]+):([^\"]+)\"", block)
                targets = [m[1].split(":")[0] for m in mounts]
                for a in targets:
                    for b in targets:
                        if a is not b and b.startswith(a.rstrip("/") + "/"):
                            self.fail(f"{b} is mounted underneath {a}")


class DocumentedTargetCountTest(unittest.TestCase):
    """No document may claim a target count that is not the one in TARGETS.

    "14 targets" survived two targets being added, in five places across three
    files, because prose has nothing checking it. A count is a fact about the
    code and it goes stale exactly the way a count in a test does -- so it gets
    the same treatment.

    THE FIRST VERSION OF THIS TEST MISSED TWENTY STALE COUNTS, which is worth
    writing down because the shape of the miss is the usual one: it read four
    files and required the number to be immediately followed by "targets" or
    "platforms". Every survivor was phrased just outside that -- "the 14-target
    CI" (a hyphen), "all 14 of the targets" (three words in between), "NOT the
    fourteen" with the noun on the next line, "Build all 16 targets" in a
    workflow_dispatch input DESCRIPTION, which is text a human reads in the
    GitHub UI -- or it was in a file the test did not open: the workflows, the
    command scripts, tools/, include/rmp/, src/main.cpp.

    So the text is NORMALISED before matching: hyphens and dashes become
    spaces, comment and table punctuation becomes spaces, and newlines become
    spaces so a claim split across a line break is one string. Every
    replacement is one character for one character, which is what lets the
    match still be reported at the right line.
    """

    WORDS = {
        "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13, "fourteen": 14,
        "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18,
        "nineteen": 19, "twenty": 20,
    }

    # PROGRESS.md is deliberately absent: it is a log, and an entry that said
    # "14 targets" in August was right in August. Rewriting history to keep a
    # gate quiet is how a log stops being worth reading. These describe the
    # present, and the present is what can be wrong.
    #
    # Every name in DOCS has to exist. CLAUDE.md sat in this list as
    # "../CLAUDE.md" -- the repository of plans, one level up -- which no CI
    # checkout has, and a missing file was skipped without a word, so the
    # document that states the count most often was never read where it
    # mattered. It lives in this repository now, and a missing one fails.
    DOCS = ("README.md", "TECHNICAL.md", "raylib_multiplatform.toml", "CLAUDE.md",
            "examples/README.md", "src/main.cpp", "rmp", "rmp.ps1", "rmp.cmd")
    GLOBS = (".github/workflows/*.yml", ".github/scripts/*.py", ".github/scripts/*.js",
             "tools/*.sh", "tools/*.py", "tests/*.py", "tests/*.h", "include/rmp/*.h",
             ".claude/skills/*/SKILL.md")

    # One character in, one character out, so offsets survive and the line
    # number a hit is reported at is the line it is actually on.
    FLATTEN = str.maketrans("\n\t-\u2013\u2014#*|/", " " * len("\n\t-\u2013\u2014#*|/"))

    def _files(self):
        seen = []
        for name in self.DOCS:
            path = REPO / name
            self.assertTrue(path.is_file(), f"{name} is gone: this test reads it")
            seen.append((name, path))
        for pattern in self.GLOBS:
            for path in sorted(REPO.glob(pattern)):
                if path.name == Path(__file__).name:
                    continue        # this file names every stale spelling
                seen.append((path.relative_to(REPO).as_posix(), path))
        return seen

    def test_no_document_states_a_stale_target_count(self):
        expected = len(cfgmod.TARGETS)
        # `(?![:\w])` after the noun: in YAML `timeout-minutes: 15` followed by
        # a `targets:` key would otherwise read, once the newline is a space,
        # as "15 targets". A key is followed by a colon; prose is not.
        pattern = re.compile(
            r"\b(\d{1,2}|" + "|".join(self.WORDS) + r")\s+"
            r"(?:(?:of|the|all|enabled|supported|shipped|build|CI|real)\s+){0,3}"
            r"(targets?|platforms?|toolchains?)(?![:\w])",
            re.IGNORECASE)

        checked = 0
        for name, path in self._files():
            text = path.read_text(encoding="utf-8", errors="replace")
            flat = text.translate(self.FLATTEN)
            self.assertEqual(len(flat), len(text))      # the offsets have to hold
            # offset -> line number, by counting the newlines in the ORIGINAL.
            starts = [0]
            for i, ch in enumerate(text):
                if ch == "\n":
                    starts.append(i + 1)
            checked += 1
            for match in pattern.finditer(flat):
                token = match.group(1).lower()
                value = self.WORDS.get(token)
                if value is None:
                    try:
                        value = int(token)
                    except ValueError:
                        continue
                if value < 10:      # "3 targets" in an example is not a claim
                    continue
                line_no = bisect.bisect_right(starts, match.start())
                line = text.splitlines()[line_no - 1] if line_no else ""
                # Historical sections describe what was true then, on purpose.
                if line.lstrip().startswith(("<summary>", "Validado con el tag")):
                    continue
                with self.subTest(doc=name, line=line_no, said=match.group(0)):
                    self.assertEqual(
                        value, expected,
                        f"{name}:{line_no} says {match.group(0)!r}, "
                        f"but TARGETS holds {expected}")
        self.assertGreater(checked, 25,
                           "this scanned almost nothing -- the globs stopped matching, "
                           "which looks exactly like a tree with no stale counts in it")

    def test_what_a_game_copies_states_no_count_at_all(self):
        """The .toml and the public headers go into every game `rmp new`
        makes, and a game turns targets off: "all seventeen targets" in its
        own .toml is then simply false. They say "every target" instead."""
        words = "|".join(self.WORDS)
        counted = re.compile(
            r"\b(\d{2}|" + words + r")\s+"
            r"(?:(?:of|the|all|enabled|supported|shipped|build|CI|real)\s+){0,3}"
            r"(targets?|platforms?|toolchains?|matrix)(?![:\w])", re.IGNORECASE)
        # In the .toml a count needs no noun beside it: "`all` here is NOT the
        # seventeen" was one. The headers use number words for other things
        # ("ten frames deep"), so there the noun has to be there.
        bare = re.compile(r"\b(" + words + r")\b", re.IGNORECASE)
        files = [REPO / "raylib_multiplatform.toml",
                 *sorted((REPO / "include" / "rmp").glob("*.h"))]
        for path in files:
            text = path.read_text(encoding="utf-8")
            flat = text.translate(self.FLATTEN)
            hits = list(counted.finditer(flat))
            if path.suffix == ".toml":
                hits += list(bare.finditer(flat))
            for match in hits:
                line_no = text.count("\n", 0, match.start()) + 1
                with self.subTest(file=path.relative_to(REPO).as_posix(), line=line_no):
                    self.fail(f"{path.name}:{line_no} counts: {match.group(0)!r}")


class ConfigureMaxDeltaTest(unittest.TestCase):
    """[app] max_delta — the longest step the game logic is ever handed.

    The bug it prevents is invisible until the machine is slow: GetFrameTime()
    returns real elapsed time and real elapsed time has no ceiling. Drag the
    window, stop at a breakpoint, let the disk stall, and it comes back half a
    second. A bullet at 900 u/s then moves 450 units in one step, through the
    wall and out the other side, having overlapped it on no frame at all.
    """

    def app(self, **overrides):
        return base_config(app=dict(copy.deepcopy(cfgmod.DEFAULTS["app"]), **overrides))

    def test_the_default_is_a_twentieth_of_a_second(self):
        """Slow enough that no normal frame is clamped, fast enough that a
        stalled one becomes a hitch instead of a teleport."""
        self.assertEqual(cfgmod.DEFAULTS["app"]["max_delta"], 0.05)

    def test_the_values_anybody_would_write_are_accepted(self):
        for value in (0.0167, 0.02, 1 / 30, 0.05, 0.1, 0.5, 1):
            with self.subTest(max_delta=value), quiet():
                cfgmod.validate(self.app(max_delta=value), False)

    def test_zero_switches_it_off_rather_than_clamping_everything_to_zero(self):
        """0 has to mean "no clamp". Read as a limit it would freeze the game
        solid, which is the opposite of what anybody typing it wants."""
        with quiet():
            cfgmod.validate(self.app(max_delta=0), False)

    def test_an_integer_is_a_number_too(self):
        with quiet():
            cfgmod.validate(self.app(max_delta=1), False)

    def test_negative_is_rejected_because_it_is_a_duration(self):
        for bad in (-0.05, -1):
            with self.subTest(max_delta=bad):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.app(max_delta=bad), False)
                self.assertIn("negative", str(caught.exception))

    def test_longer_than_a_second_is_rejected_and_says_what_to_use(self):
        for bad in (1.5, 10, 3600):
            with self.subTest(max_delta=bad):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.app(max_delta=bad), False)
                message = str(caught.exception)
                self.assertIn("0.0167", message)
                self.assertIn("0", message)

    def test_shorter_than_a_frame_at_240_hz_is_rejected(self):
        """Every frame would be clamped and the game would run in permanent
        slow motion. Nobody means that, so it is a typo, and a typo that
        compiles is the expensive kind."""
        for bad in (0.001, 0.0001, 1e-9):
            with self.subTest(max_delta=bad):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.app(max_delta=bad), False)
                self.assertIn("slow", str(caught.exception))

    def test_it_has_to_be_a_number(self):
        """`max_delta = true` is the one that matters: in Python True is an int
        and it would otherwise sail through as a one-second clamp."""
        for bad in (True, False, "0.05", None, [0.05], {"s": 1}):
            with self.subTest(max_delta=bad):
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    cfgmod.validate(self.app(max_delta=bad), False)

    def test_it_reaches_the_runners_and_not_just_the_header(self):
        """A clamp nothing applies is a clamp that does nothing. Every entry
        point has to go through step_delta() rather than GetFrameTime(), or a
        game built with RMP_ENTRY_POINT gets the raw number."""
        app_h = (REPO / "include" / "rmp" / "app.h").read_text()
        self.assertNotIn("FRAME(GetFrameTime())", app_h)
        self.assertEqual(app_h.count("FRAME(rmp::app::detail::step_delta())"), 3)
        self.assertIn("RMP_MAX_DELTA", (REPO / "src" / "rmp" / "app.cpp").read_text())


def load_license_db():
    """tools/license_db.py, by path, the same way configure.py is loaded."""
    spec = importlib.util.spec_from_file_location("rmp_license_db", REPO / "tools" / "license_db.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["rmp_license_db"] = module
    spec.loader.exec_module(module)
    return module


ldb = load_license_db()

FIXTURES = REPO / "tests" / "fixtures" / "licenses"


def row(name, path, licences, elect="-", modified="no", linked="all", evidence="file"):
    return ldb.Row(name=name, path=path, licences=licences, elect=elect,
                   modified=modified, linked=linked, evidence=evidence)


class LicenceGuardTest(unittest.TestCase):
    """tools/license_db.py --check, proven red on each thing it was written for.

    The guard is a comparison of three things -- the tree, the record in
    THIRD_PARTY_LICENSES.md, and the licence texts -- so every test here is a
    small tree under tests/fixtures/licenses/ plus the rows that describe it,
    checked with the walker pointed at the fixture instead of thirdparty/. The
    real tree is the last test, and it is the definition of done.
    """

    def check(self, rows, root, pins=None):
        # The fixture root stands in for thirdparty/; the packaging files are
        # the real ones, which is fine: they are not what a fixture varies.
        _, fails = ldb.check(rows, pins or {}, repo=REPO, root=root, families=set())
        return fails

    def fixture(self, name):
        """A root holding exactly one component, `dep/`, under the named case."""
        return FIXTURES / name

    def rows_for_one(self, name, **kw):
        # Paths in a row are repo-relative; the fixture dirs are under tests/.
        rel = str((FIXTURES / name / "dep").relative_to(REPO))
        return [row(name, rel, **kw)]

    def test_gpl_is_refused_by_name(self):
        fails = self.check(self.rows_for_one("gpl_dep", licences="GPL"), self.fixture("gpl_dep"))
        self.assertTrue(any("GPL" in f and "copyleft" in f for f in fails), fails)

    def test_lgpl_is_refused_separately_from_gpl(self):
        # A fingerprint that matched "general public license" before checking
        # for "lesser" would report this as GPL; the message has to say LGPL
        # and say why it is refused here (no dynamic linking on iOS/musl).
        fails = self.check(self.rows_for_one("lgpl_dep", licences="LGPL"), self.fixture("lgpl_dep"))
        self.assertTrue(any("LGPL" in f and "dynamic linking" in f for f in fails), fails)
        self.assertEqual(ldb.classify((FIXTURES / "lgpl_dep" / "dep" / "LICENSE").read_text()), ["LGPL"])

    def test_no_licence_at_all_is_refused(self):
        fails = self.check(self.rows_for_one("mystery_dep", licences="MIT"),
                           self.fixture("mystery_dep"))
        self.assertTrue(any("no licence text found" in f for f in fails), fails)
        # And a row that admits it: UNKNOWN is refused outright.
        fails = self.check(self.rows_for_one("mystery_dep", licences="UNKNOWN"),
                           self.fixture("mystery_dep"))
        self.assertTrue(any("UNKNOWN" in f and "no recognisable licence" in f for f in fails), fails)

    def test_non_commercial_is_refused(self):
        fails = self.check(self.rows_for_one("cc_by_nc", licences="CC-BY-NC"),
                           self.fixture("cc_by_nc"))
        self.assertTrue(any("non-commercial" in f for f in fails), fails)

    def test_a_row_cannot_lie_about_the_text(self):
        # The text says GPL; the row says MIT. The guard reads the text.
        fails = self.check(self.rows_for_one("gpl_dep", licences="MIT"), self.fixture("gpl_dep"))
        self.assertTrue(any("reads as GPL" in f for f in fails), fails)

    def test_zlib_unmarked_and_unpinned_is_refused(self):
        # A single-header zlib component at the top level is either pinned by
        # sha256 (unmodified, and provably so) or marked modified with a
        # PATCHES.md. Neither is the case that lets an alteration go unmarked.
        fails = self.check(self.rows_for_one("zlib_unmarked", licences="zlib"),
                           self.fixture("zlib_unmarked"))
        self.assertTrue(any("no pin" in f and "sha256_zlib_unmarked" in f for f in fails), fails)
        # Marked as modified without the file that is the mark: still refused.
        fails = self.check(self.rows_for_one("zlib_unmarked", licences="zlib", modified="yes"),
                           self.fixture("zlib_unmarked"))
        self.assertTrue(any("PATCHES.md" in f and "missing" in f for f in fails), fails)

    def test_zlib_marked_passes_and_so_does_a_true_pin(self):
        fails = self.check(self.rows_for_one("zlib_marked", licences="zlib", modified="yes"),
                           self.fixture("zlib_marked"))
        self.assertEqual(fails, [])
        header = FIXTURES / "zlib_unmarked" / "dep" / "thing.h"
        pins = {"sha256_zlib_unmarked": ldb.sha256_of(header)}
        fails = self.check(self.rows_for_one("zlib_unmarked", licences="zlib"),
                           self.fixture("zlib_unmarked"), pins)
        self.assertEqual(fails, [])
        # A pin that no longer matches is a modification nobody marked.
        pins = {"sha256_zlib_unmarked": "0" * 64}
        fails = self.check(self.rows_for_one("zlib_unmarked", licences="zlib"),
                           self.fixture("zlib_unmarked"), pins)
        self.assertTrue(any("does not match the pin" in f for f in fails), fails)

    def test_a_component_of_several_files_is_pinned_too(self):
        # rres (two headers) and cJSON (a .c and a .h) used to be "unmodified"
        # on trust, because only one-file components had a pin to check.
        rows = self.rows_for_one("two_file_dep", licences="MIT")
        fails = self.check(rows, self.fixture("two_file_dep"))
        self.assertTrue(any("no pin" in f and "sha256_two_file_dep" in f for f in fails), fails)
        dep = FIXTURES / "two_file_dep" / "dep"
        sources = ldb.pinned_sources(dep)
        self.assertEqual([p.name for p in sources], ["thing.c", "thing.h"])
        good = {"sha256_two_file_dep": ldb.pin_of(sources, dep)}
        self.assertEqual(self.check(rows, self.fixture("two_file_dep"), good), [])
        # The pin is over BOTH files: the hash of either one alone is stale.
        for alone in sources:
            with self.subTest(pinned_only=alone.name):
                fails = self.check(rows, self.fixture("two_file_dep"),
                                   {"sha256_two_file_dep": ldb.sha256_of(alone)})
                self.assertTrue(any("does not match the pin" in f for f in fails), fails)
        # And a renamed file moves it even with the same bytes.
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            shutil.copy(dep / "thing.h", Path(tmp) / "thing.h")
            shutil.copy(dep / "thing.c", Path(tmp) / "other.c")
            self.assertNotEqual(ldb.pin_of(ldb.pinned_sources(Path(tmp)), Path(tmp)),
                                ldb.pin_of(sources, dep))

    def test_what_a_component_bundles_is_in_its_pin(self):
        # rres/external/ holds the monocypher rmp::save seals with; a pin of
        # the top level alone let a line appended to monocypher.c pass.
        dep = FIXTURES / "nested_dep" / "dep"
        sources = ldb.pinned_sources(dep)
        self.assertEqual([p.relative_to(dep).as_posix() for p in sources],
                         ["sub/bundled.c", "top.h"])
        rows = self.rows_for_one("nested_dep", licences="MIT")
        pin = {"sha256_nested_dep": ldb.pin_of(sources, dep)}
        self.assertEqual(self.check(rows, self.fixture("nested_dep"), pin), [])
        # The top file alone is a stale pin now.
        stale = {"sha256_nested_dep": ldb.sha256_of(dep / "top.h")}
        fails = self.check(rows, self.fixture("nested_dep"), stale)
        self.assertTrue(any("does not match the pin" in f for f in fails), fails)
        # And a change one level down moves it.
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "dep"
            shutil.copytree(dep, copy)
            with (copy / "sub" / "bundled.c").open("a") as f:
                f.write("/* one more line */\n")
            self.assertNotEqual(ldb.pin_of(ldb.pinned_sources(copy), copy),
                                ldb.pin_of(sources, dep))

    def test_a_bundled_file_we_changed_says_so_where_its_owner_does(self):
        # rlsw.h, inside raylib, carries a backport. Its own row says
        # modified, and raylib's PATCHES.md -- the mark of the component that
        # bundles it -- has to name the file, or the row is a claim nobody
        # recorded.
        top = str((FIXTURES / "bundled_modified" / "dep").relative_to(REPO))
        rows = [row("bundled_modified", top, "zlib", modified="yes"),
                row("inner", top + "/external/inner.h", "MIT", modified="yes", evidence="header")]
        self.assertEqual(self.check(rows, self.fixture("bundled_modified")), [])
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory(dir=FIXTURES) as tmp:
            case = Path(tmp)
            shutil.copytree(FIXTURES / "bundled_modified" / "dep", case / "dep")
            (case / "dep" / "PATCHES.md").write_text("# This is a MODIFIED copy of thing\n")
            rel = str((case / "dep").relative_to(REPO))
            rows = [row("bundled_modified", rel, "zlib", modified="yes"),
                    row("inner", rel + "/external/inner.h", "MIT", modified="yes", evidence="header")]
            fails = self.check(rows, case)
            self.assertTrue(any("inner" in f and "naming external/inner.h" in f for f in fails), fails)

    def test_one_file_keeps_the_plain_sha256(self):
        # So a one-file pin can still be checked with sha256sum by hand.
        header = FIXTURES / "mit_dep" / "dep" / "thing.h"
        self.assertEqual(ldb.pin_of([header]), ldb.sha256_of(header))

    def test_a_pin_with_no_row_is_refused(self):
        rows = self.rows_for_one("zlib_marked", licences="zlib", modified="yes")
        fails = self.check(rows, self.fixture("zlib_marked"),
                           {"sha256_doctest": "a" * 64})
        self.assertTrue(any("sha256_doctest" in f and "no row" in f for f in fails), fails)
        # A pin whose row is there is not an orphan.
        fails = self.check(rows, self.fixture("zlib_marked"), {"sha256_zlib_marked": "a" * 64})
        self.assertFalse(any("no row" in f for f in fails), fails)

    def test_the_framework_notice_ships_with_every_family(self):
        """The framework's own MIT notice was in no LICENSES.txt at all. (iOS's
        needs the raylib-ios submodule checked out; it reads the same rows.)"""
        for family in ("desktop", "android"):
            with self.subTest(family=family):
                text = cfgmod.licenses_text(base_config(), family)
                self.assertIn("raylib_multiplatform -- MIT", text)
                self.assertIn("Copyright (c) 2026 omardev29", text)
                self.assertIn("The framework this game is built on", text)

    def test_the_web_package_has_a_notice_of_its_own(self):
        """The web package shipped the DESKTOP LICENSES.txt, crediting GLFW,
        RGFW, glad and the Windows shims, none of which is in a .wasm: the
        browser build talks to Emscripten's JavaScript GLFW, not GLFW's C. The
        web notice lists what the web build contains, and the web job ships it."""
        self.assertIn("web", cfgmod.LICENSE_FILES)
        rows = ldb.load_rows(REPO)
        text = cfgmod.licenses_text(base_config(), "web", rows)
        desktop_only = [r for r in rows if r.linked == {"desktop"}]
        self.assertGreater(len(desktop_only), 3)
        for r in desktop_only:
            with self.subTest(absent=r["name"]):
                self.assertNotIn(f"  {r['name']} --", text)
        for r in ldb.rows_for(rows, "web"):
            if r["evidence"].startswith("part-of:"):
                continue  # credited through its parent's notice
            with self.subTest(present=r["name"]):
                self.assertIn(f"  {r['name']} --", text)
        self.assertIn("raylib_multiplatform -- MIT", text)
        web = (REPO / ".github" / "workflows" / "_web.yml").read_text()
        self.assertIn("cmake/generated/web/LICENSES.txt", web)
        self.assertNotIn("cp cmake/generated/LICENSES.txt build/web/", web)

    def test_the_symmetry_both_ways(self):
        root = FIXTURES / "symmetry"
        rel = lambda n: str((root / n).relative_to(REPO))
        rows = [row("present", rel("present"), "MIT"), row("ghost", rel("ghost"), "MIT")]
        pins = {"sha256_present": ldb.sha256_of(root / "present" / "a.h")}
        fails = self.check(rows, root, pins)
        self.assertTrue(any("unlisted" in f and "not in the components block" in f for f in fails), fails)
        self.assertTrue(any("ghost" in f and "not on disk" in f for f in fails), fails)
        # Both directions, and nothing about `present`, which is in order.
        self.assertFalse(any(f.startswith("present") for f in fails), fails)

    def test_a_good_component_passes(self):
        # A guard that has only been seen red on the bad cases has not been
        # seen green on the good one.
        pins = {"sha256_mit_dep": ldb.sha256_of(FIXTURES / "mit_dep" / "dep" / "thing.h")}
        fails = self.check(self.rows_for_one("mit_dep", licences="MIT"), self.fixture("mit_dep"), pins)
        self.assertEqual(fails, [])

    def test_the_block_is_validated_as_it_is_parsed(self):
        with self.assertRaises(ValueError) as caught:
            ldb.parse_block("```components\nx thirdparty/x MIT - no all\n```")
        self.assertIn("columns", str(caught.exception))
        with self.assertRaises(ValueError) as caught:
            ldb.parse_block("```components\nx thirdparty/x MIT|zlib - no all file\n```")
        self.assertIn("elect", str(caught.exception))
        with self.assertRaises(ValueError) as caught:
            ldb.parse_block("```components\nx thirdparty/x MIT|zlib BSD-2 no all file\n```")
        self.assertIn("not one of its licences", str(caught.exception))
        with self.assertRaises(ValueError):
            ldb.parse_block("```components\nx thirdparty/x MIT - maybe all file\n```")
        with self.assertRaises(ValueError):
            ldb.parse_block("```components\nx thirdparty/x MIT - no everywhere file\n```")
        with self.assertRaises(ValueError):
            ldb.parse_block("no block here")

    def test_fingerprints_tell_the_families_apart(self):
        cases = {
            "This is free and unencumbered software released into the public domain.": ["Unlicense", "public-domain"],
            "SPDX-License-Identifier: BSD-2-Clause OR CC0-1.0\nRedistributions in binary form must": ["CC0", "BSD-2"],
            "Neither the name of X nor ... Redistributions in binary form": ["BSD-3"],
            "Altered source versions must be plainly marked as such": ["zlib"],
            "Permission is hereby granted, free of charge, to any person": ["MIT"],
            "Licensed under the Apache License, Version 2.0": ["Apache-2.0"],
            "Do What The Fuck You Want To Public License": ["WTFPL"],
            "Permission to use, copy, modify, and distribute this software and its documentation": ["permissive"],
            "GNU Affero General Public License": ["AGPL"],
            "nothing at all": [],
        }
        for text, families in cases.items():
            with self.subTest(text=text[:40]):
                self.assertEqual(ldb.classify(text), families)
        # "commercial or non-commercial" in a public-domain dedication is NOT
        # CC BY-NC; the phrase needs Creative Commons next to it.
        self.assertNotIn("CC-BY-NC", ldb.classify(
            "for any purpose, commercial or non-commercial, and by any means."))

    def test_the_packaging_table_covers_every_family(self):
        families = {family for family, *_ in cfgmod.TARGETS.values()}
        # ios is a target of the apple family and packages separately.
        self.assertEqual(set(ldb.FAMILY_FILES), families | {"ios"})

    def test_a_crlf_checkout_keeps_its_pins(self):
        """The second thing a Windows checkout broke: three pinned components
        hashed differently with CRLF line endings, and `rmp new` refused the
        game it had just made."""
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            lf, crlf = Path(tmp) / "lf.h", Path(tmp) / "crlf.h"
            lf.write_bytes(b"int a;\nint b;\n")
            crlf.write_bytes(b"int a;\r\nint b;\r\n")
            self.assertEqual(ldb.pin_of([lf]), ldb.pin_of([crlf]))
            self.assertEqual(ldb.pin_of([lf]), hashlib.sha256(b"int a;\nint b;\n").hexdigest())
        # And no pinned file is stored with CRLF, so sha256sum on Linux agrees.
        import subprocess
        for name in ("cute_aseprite", "rres", "cJSON"):
            for f in (REPO / "thirdparty" / name).rglob("*"):
                if f.is_file() and f.suffix in (".c", ".h"):
                    with self.subTest(file=str(f.relative_to(REPO))):
                        self.assertNotIn(b"\r\n", f.read_bytes())

    def test_a_windows_checkout_names_the_same_components(self):
        """On Windows the walker's paths print with backslashes, and the block
        is written with slashes: every component read as missing from it, so
        `rmp test config` and `rmp new` failed on every Windows machine. The
        Windows rmp job found it. Here the walker is made to return Windows
        paths, which is what it does there."""
        import pathlib
        real = ldb.inventory
        ldb.inventory = lambda root, repo=REPO: [pathlib.PureWindowsPath(p)
                                                 for p in real(root, repo)]
        try:
            _, fails = ldb.check(ldb.load_rows(), ldb.frozen_pins(), repo=REPO)
        finally:
            ldb.inventory = real
        self.assertEqual([f for f in fails if "on disk, but not" in f], [])

    def test_the_real_tree_passes(self):
        """The definition of done: every component under thirdparty/ has a row,
        a licence we ship under, its text where the row says, and a mark on
        every alteration -- and it stays that way, because this runs in
        `rmp test` and in the lint job."""
        import subprocess
        run = subprocess.run([sys.executable, str(REPO / "tools" / "license_db.py"), "--check"],
                             capture_output=True, text=True, cwd=REPO)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS:", run.stdout)
        # And it saw the whole tree, not a subset: raylib's bundled deps included.
        self.assertIn("stb_image", run.stdout)
        self.assertIn("packaging", run.stdout)

    def test_every_alteration_has_its_mark(self):
        rows = ldb.load_rows(REPO)
        marked = {r["name"] for r in rows if r["modified"] in ("yes", "subset")}
        self.assertEqual(marked, {"raylib", "clay", "cute_tiled", "raylib-cpp", "raymob", "rlsw"})
        for r in rows:
            if r["modified"] in ("yes", "subset"):
                with self.subTest(component=r["name"]):
                    path = REPO / r["path"]
                    if path.is_dir():
                        note = (path / "PATCHES.md").read_text()
                    else:
                        # A bundled file: its change is in the bundling
                        # component's PATCHES.md, by name.
                        owner = next(d for d in path.parents if (d / "PATCHES.md").is_file())
                        note = (owner / "PATCHES.md").read_text()
                        self.assertIn(f"`{path.relative_to(owner).as_posix()}`", note)
                    self.assertIn("MODIFIED", note)
                    self.assertIn("|", note)  # a table of file/line/change/why

    def test_a_comment_over_rows_names_every_modified_one_under_it(self):
        """The comment over raylib's bundled libraries said none of them is
        modified, three lines above rlsw's `yes`. A comment that heads a group
        of rows and talks about modification names each modified row in it,
        and says "none" only when none is."""
        text = (REPO / "THIRD_PARTY_LICENSES.md").read_text()
        block = text[text.index("```components"):]
        block = block[:block.index("\n```\n", 1)]
        groups, comment, after_rows = [], [], True
        for line in block.splitlines()[1:]:
            if line.lstrip().startswith("#"):
                if line.split()[1:2] == ["name"] and "evidence" in line:
                    continue  # the column header
                if after_rows:
                    groups.append((comment := [], rows := []))
                    after_rows = False
                comment.append(line.lstrip("# "))
            elif line.strip():
                after_rows = True
                if groups:
                    groups[-1][1].append(line.split())
        checked = 0
        for comment, rows in groups:
            prose = " ".join(comment)
            if "modified" not in prose:
                continue
            checked += 1
            changed = [r[0] for r in rows if r[4] in ("yes", "subset")]
            with self.subTest(comment=prose[:60]):
                for name in changed:
                    self.assertIn(name, prose)
                if changed:
                    self.assertNotRegex(prose, r"\bnone of them is modified\b")
        self.assertGreater(checked, 0)

    def test_the_raylib_patch_sites_are_where_patches_md_says(self):
        # The mark is only a mark if it is true: every site PATCHES.md names
        # carries the PATCHED comment, and every PATCHED comment is listed.
        note = (REPO / "thirdparty" / "raylib" / "PATCHES.md").read_text()
        listed = set(re.findall(r"^\| `([^`]+)` \|", note, re.M))
        marked = set()
        for path in (REPO / "thirdparty" / "raylib").rglob("*"):
            if path.is_file() and path.suffix in (".c", ".h", ".txt", ".cmake"):
                if "PATCHED (raylib_multiplatform)" in path.read_text(errors="replace"):
                    marked.add(str(path.relative_to(REPO / "thirdparty" / "raylib")))
        self.assertEqual(marked, listed)


class ConfigureLicencesTest(unittest.TestCase):
    """[deploy] licenses, and the LICENSES.txt files gen_licenses() writes."""

    def test_licenses_must_be_a_bool(self):
        for bad in ("true", 1, 0, None, [True]):
            with self.subTest(licenses=bad):
                cfg = base_config()
                cfg["deploy"]["licenses"] = bad
                with self.assertRaises(cfgmod.ConfigError), quiet():
                    cfgmod.validate(cfg, False)

    def test_turning_the_notice_off_needs_a_reason_on_record(self):
        cfg = base_config()
        cfg["deploy"]["licenses"] = False
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        self.assertIn("credits_note", str(caught.exception))
        self.assertIsNotNone(cfgmod.locate_from(caught.exception))
        cfg["deploy"]["credits_note"] = "   "
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(cfg, False)
        cfg["deploy"]["credits_note"] = "the Credits scene, from the main menu"
        with quiet():
            cfgmod.validate(cfg, False)
        cfg["deploy"]["credits_note"] = ["a list"]
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(cfg, False)

    @contextlib.contextmanager
    def captured_writes(self):
        """What the generators would write, by path under the checkout --
        with the notices pointed at a temporary tree, because gen_licenses()
        also DELETES them, and that does not go through write()."""
        captured = {}
        original = cfgmod.write

        def capture(path, content):
            captured[str(path.relative_to(self._root if self._root in path.parents else REPO))] = content
            return True
        cfgmod.write = capture
        with licence_files_in() as root:
            self._root = root
            try:
                yield captured
            finally:
                cfgmod.write = original

    _root = REPO

    def test_off_removes_the_notices_and_says_it_changed_them(self):
        cfg = base_config()
        cfg["deploy"]["licenses"] = False
        cfg["deploy"]["credits_note"] = "the Credits scene"
        with licence_files_in() as root, quiet():
            for out in cfgmod.LICENSE_FILES.values():
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text("stale")
            self.assertIs(cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"]), True)
            self.assertEqual(sorted(p for p in root.rglob("*") if p.is_file()), [])
            self.assertIs(cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"]), False,
                          "nothing left to remove is not a change")

    def test_on_says_whether_it_wrote_anything(self):
        """`changed = write(...) or changed`, with write() returning None, was
        False after every configure."""
        cfg = base_config()
        with licence_files_in() as root, quiet():
            self.assertIs(cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"]), True)
            self.assertTrue((root / "cmake" / "generated" / "LICENSES.txt").is_file())
            self.assertIs(cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"]), False)
            cfg["project"]["name"] = "renamed"
            self.assertIs(cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"]), True)

    def test_write_says_whether_it_wrote(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a" / "b.txt"
            self.assertIs(cfgmod.write(path, "one"), True)
            self.assertIs(cfgmod.write(path, "one"), False)
            self.assertIs(cfgmod.write(path, "two"), True)

    def test_each_family_gets_what_it_links_and_nothing_else(self):
        cfg = base_config()
        with self.captured_writes() as out, quiet():
            cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"])
        desktop = out["cmake/generated/LICENSES.txt"]
        android = out["cmake/generated/android/LICENSES.txt"]
        self.assertNotIn("cmake/generated/ios/LICENSES.txt", out)  # ios not a target
        # raymob is Android glue: in the APK notice, not next to a Linux binary.
        self.assertNotIn("  raymob --", desktop)
        self.assertIn("  raymob --", android)
        # doctest is never shipped; lz4 is in the tree and not compiled.
        for text in (desktop, android):
            self.assertNotIn("doctest", text)
            self.assertNotIn("  lz4 --", text)
            # raylib's bundled dependencies, which the old generator never saw.
            self.assertIn("  stb_image --", text)
            self.assertIn("Sean Barrett", text)
            self.assertIn("  miniaudio --", text)
            self.assertIn("David Reid", text)
            self.assertIn("  monocypher --", text)
            self.assertIn("  qoi --", text)
            self.assertIn("Dominic Szablewski", text)
            self.assertIn("tiny-AES-c", text)
            # The marks and the elections, in the shipped file.
            self.assertIn("  raylib -- zlib\n  MODIFIED for this build. See thirdparty/raylib/PATCHES.md", text)
            self.assertIn("  clay -- zlib\n  MODIFIED", text)
            self.assertIn("  raylib-cpp -- zlib\n  PARTIAL copy", text)
            self.assertIn("this build takes the Unlicense alternative", text)
            self.assertIn("not\naffiliated with or endorsed by the raylib project", text)
            self.assertIn("Ramon Santamaria", text)
        # GLFW is desktop-only; the Android notice does not credit it.
        self.assertIn("  glfw --", desktop)
        self.assertNotIn("  glfw --", android)

    def test_ios_without_the_submodule_warns_locally_and_fails_on_the_job(self):
        cfg = base_config()
        fork = REPO / "thirdparty" / "raylib-ios"
        if fork.is_dir() and any(fork.iterdir()):
            self.skipTest("the raylib-ios submodule is checked out here")
        cfgmod._warnings.clear()
        with self.captured_writes() as out, quiet():
            cfgmod.gen_licenses(cfg, ["ios"])
        self.assertNotIn("cmake/generated/ios/LICENSES.txt", out)
        self.assertTrue(any("raylib-ios is empty" in w for w in cfgmod._warnings), cfgmod._warnings)
        cfgmod._warnings.clear()
        with self.assertRaises(cfgmod.ConfigError) as caught, self.captured_writes(), quiet():
            cfgmod.gen_licenses(cfg, ["ios"], require_notices=True)
        self.assertIn("git submodule update --init", str(caught.exception))
        # And the iOS job is the one that asks for that.
        apple = (REPO / ".github" / "workflows" / "_apple.yml").read_text()
        self.assertIn("configure.py --require-notices", apple)

    def test_off_means_no_files_at_all(self):
        cfg = base_config()
        cfg["deploy"]["licenses"] = False
        cfg["deploy"]["credits_note"] = "the Credits scene"
        with self.captured_writes() as out, quiet():
            cfgmod.gen_licenses(cfg, ["linux-x64-glibc", "android"])
        self.assertEqual(out, {})

    def test_the_ios_project_carries_the_notice_only_when_it_exists(self):
        cfg = base_config()
        with self.captured_writes() as out, quiet():
            cfgmod.gen_ios_project(cfg)
        project = out["ios/project.yml"]
        if cfgmod.LICENSE_FILES["ios"].is_file():
            self.assertIn("../cmake/generated/ios/LICENSES.txt", project)
        else:
            self.assertNotIn("LICENSES.txt", project)
        # The generator knows how to reference it either way.
        self.assertIn("cmake/generated/ios/LICENSES.txt", (REPO / "tools" / "configure.py").read_text())

    def test_the_apk_gets_its_notice(self):
        gradle = (REPO / "raymob" / "app" / "build.gradle").read_text()
        self.assertIn("cmake/generated/android/LICENSES.txt", gradle)
        self.assertIn('new File(destDir, "LICENSES.txt")', gradle)

    def test_the_readme_no_longer_calls_raylib_unchanged(self):
        readme = (REPO / "README.md").read_text()
        self.assertNotIn("unchanged", readme.split("### What we add on top of raylib")[1].split("|")[0])
        self.assertIn("thirdparty/raylib/PATCHES.md", readme)
        self.assertIn("not\naffiliated", readme.replace("is not\naffiliated", "not\naffiliated"))


class DocumentedTreeTest(unittest.TestCase):
    """The documents describe the tree that exists.

    TECHNICAL.md's directory tree was three phases behind and README.md's
    header list two, and nothing said so, because prose does not fail a build.
    These compare the two against the disk: every public header and every
    framework source is named, and nothing named is gone.
    """

    def tree_block(self):
        text = (REPO / "TECHNICAL.md").read_text()
        start = text.index("raylib_multiplatform.toml # THE config.")
        end = text.index("FROZEN_VERSIONS.md", start)
        return text[start:end]

    def test_every_public_header_is_in_the_tree_and_the_readme(self):
        headers = sorted(p.name for p in (REPO / "include" / "rmp").glob("*.h"))
        self.assertGreater(len(headers), 8)
        tree = self.tree_block()
        readme = (REPO / "README.md").read_text()
        for name in headers:
            with self.subTest(header=name):
                self.assertRegex(tree, r"[│ ]*(├|└)── " + re.escape(name) + r"\s",
                                 f"TECHNICAL.md's tree does not list include/rmp/{name}")
                self.assertIn(f"`rmp/{name}`", readme,
                              f"README.md's header list does not name rmp/{name}")

    def test_every_framework_source_is_in_the_tree(self):
        sources = sorted(str(p.relative_to(REPO / "src" / "rmp"))
                         for p in (REPO / "src" / "rmp").rglob("*")
                         if p.is_file() and p.suffix in (".cpp", ".h"))
        self.assertGreater(len(sources), 20)
        tree = self.tree_block()
        for rel in sources:
            with self.subTest(source=rel):
                self.assertRegex(tree, r"(├|└)── " + re.escape(Path(rel).name) + r"\s",
                                 f"TECHNICAL.md's tree does not list src/rmp/{rel}")

    def test_nothing_in_the_tree_is_gone(self):
        # Every source, script and CMake file the tree names exists.
        tree = self.tree_block()
        named = re.findall(r"(?:├|└)── ([A-Za-z_]+\.(?:h|cpp|c|py|sh|cmake))\s", tree)
        self.assertGreater(len(named), 40)
        on_disk = {p.name for p in (REPO / "include" / "rmp").glob("*.h")}
        on_disk |= {p.name for p in (REPO / "src" / "rmp").rglob("*") if p.is_file()}
        on_disk |= {p.name for p in (REPO / "tests").glob("*")}
        on_disk |= {p.name for p in (REPO / "tools").glob("*")}
        on_disk |= {p.name for p in (REPO / "cmake").glob("*")}
        on_disk |= {p.name for p in (REPO / "src").glob("*")}
        for name in named:
            with self.subTest(name=name):
                self.assertIn(name, on_disk, f"TECHNICAL.md's tree names {name}, which does not exist")

    # Each public header and the module it is, as the README's table names it.
    # A header missing from this map fails below, which is what makes a new
    # module (rmp/audio.h and rmp/save.h were missing from the table for a
    # whole phase, behind a hard-coded list) add its README row.
    MODULES = {"app.h": ["rmp::app"], "scene.h": ["rmp::Scene", "rmp::Camera"],
               "object.h": ["rmp::Object"], "behavior.h": ["rmp::behavior"],
               "input.h": ["rmp::input"], "ui.h": ["rmp::ui"], "assets.h": ["rmp::assets"],
               "tilemap.h": ["rmp::Tilemap"], "audio.h": ["rmp::audio"], "save.h": ["rmp::save"],
               "random.h": ["rmp::random"], "ads.h": ["rmp::ads"],
               "math.h": [], "config.h": []}  # math and config are not modules with a row

    def test_the_readme_namespace_table_names_every_module(self):
        readme = (REPO / "README.md").read_text()
        headers = {p.name for p in (REPO / "include" / "rmp").glob("*.h")}
        self.assertEqual(headers - set(self.MODULES), set(),
                         "a public header with no entry in MODULES -- add it, and its README row")
        for header, modules in self.MODULES.items():
            for module in modules:
                with self.subTest(header=header, module=module):
                    self.assertIn(f"| **`{module}`** |", readme)


class NoAbsoluteIncludeTest(unittest.TestCase):
    """No source includes a file by absolute path.

    An agent left `#include "/tmp/.../probe.h"` at the top of a game while
    driving it headless; it compiled on the machine that had the file and
    took the examples job and the MSVC pass red twenty minutes after the push.
    A path that starts with / (or a drive letter) is never something the
    repository can promise exists.
    """

    ROOTS = ("src", "include", "tests", "examples")

    def test_every_include_is_relative(self):
        offenders = []
        for root in self.ROOTS:
            for path in (REPO / root).rglob("*"):
                if path.suffix not in (".c", ".cpp", ".h", ".hpp") or not path.is_file():
                    continue
                for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                    if re.match(r'\s*#\s*include\s+"(/|[A-Za-z]:[\\/])', line):
                        offenders.append(f"{path.relative_to(REPO)}:{number}: {line.strip()}")
        self.assertEqual(offenders, [], "absolute #include paths:\n" + "\n".join(offenders))


class VendoredHeaderPathsTest(unittest.TestCase):
    """A vendored dependency has to be on the include path of EVERY build.

    There are five, and getting four of them is indistinguishable from getting
    all five until a runner says otherwise twenty minutes later. That is exactly
    what happened with cute_tiled: CMake had it, and the iOS job -- which builds
    through a generated Xcode project with its own HEADER_SEARCH_PATHS -- came
    back with `fatal error: 'cute_tiled.h' file not found`.

    There were five. The examples job and the old command script carried
    their own -I lists until the examples became CMake targets, which left four -- and
    added one the gate had never looked at:
      CMakeLists.txt                        desktop, BSD, Web, and every example
      raymob/app/src/main/cpp/CMakeLists.txt Android, which has its own
      tools/configure.py                    iOS, through XcodeGen
      .github/workflows/_windows.yml        the MSVC syntax pass over examples/
    """

    # Directories under thirdparty/ that are NOT include roots: the ones whose
    # headers are reached through a path prefix, or that hold no header at all.
    # raylib-cpp is reached as <raylib-cpp/Vector2.hpp> through the BARE
    # thirdparty root, which the second test below checks for by itself.
    NOT_INCLUDE_ROOTS = {"raylib", "raylib-ios", "raylib-cpp", "raymob", "doctest"}

    BUILDS = {
        "CMakeLists.txt": "CMakeLists.txt",
        "Android CMakeLists": "raymob/app/src/main/cpp/CMakeLists.txt",
        "iOS (configure.py)": "tools/configure.py",
        "the MSVC examples pass": ".github/workflows/_windows.yml",
    }

    # The bare thirdparty root, spelled the way each build spells an include
    # directory. rmp/math.h includes <raylib-cpp/Color.hpp>, so a build without
    # this root cannot compile that header -- and Android, iOS and the MSVC pass
    # all lacked it for a phase before anything noticed.
    BARE_ROOT = {
        "CMakeLists.txt": r'/thirdparty"',
        "Android CMakeLists": r'/thirdparty"',
        "iOS (configure.py)": r'\.\./thirdparty\n',
        "the MSVC examples pass": r'"thirdparty"',
    }

    def vendored(self):
        root = REPO / "thirdparty"
        out = []
        for entry in sorted(root.iterdir()):
            if not entry.is_dir() or entry.name in self.NOT_INCLUDE_ROOTS:
                continue
            if not any(entry.glob("*.h")) and not any(entry.glob("*.hpp")):
                continue
            out.append(entry.name)
        return out

    def test_there_is_more_than_one_vendored_include_root(self):
        """If this ever finds none, the test below is passing vacuously."""
        self.assertGreater(len(self.vendored()), 1, self.vendored())

    def test_every_vendored_header_directory_is_on_every_include_path(self):
        for name in self.vendored():
            for build, path in self.BUILDS.items():
                with self.subTest(dependency=name, build=build):
                    # PowerShell spells the MSVC list with backslashes.
                    text = (REPO / path).read_text().replace("\\", "/")
                    self.assertIn("thirdparty/" + name, text,
                                  name + " is not on the include path in " + path +
                                  " -- a build that cannot find its header fails at "
                                  "the first file that includes it, and only on the "
                                  "platform that uses that build")

    def test_the_bare_thirdparty_root_is_on_every_include_path(self):
        for build, path in self.BUILDS.items():
            with self.subTest(build=build):
                text = (REPO / path).read_text()
                self.assertRegex(text, self.BARE_ROOT[build],
                                 "the bare thirdparty/ root is not on the include path in " +
                                 path + " -- rmp/math.h includes <raylib-cpp/*.hpp> through it")

    def test_no_build_carries_its_own_examples_include_list(self):
        """The examples job and the command script used to repeat the -I list
        by hand.

        They compile the examples through CMake now, so a -I list reappearing in
        either is a sixth copy of something that already lives on the `rmp`
        target -- and a copy is what drifts.
        """
        for path in (".github/workflows/ci.yml", "tools/rmp.py"):
            with self.subTest(file=path):
                text = (REPO / path).read_text()
                self.assertNotIn("-Ithirdparty/cute_tiled", text,
                                 path + " has grown its own include list again; "
                                 "examples are CMake targets, build them that way")

    def test_no_two_examples_share_a_header_name(self):
        """The MSVC pass over examples/ puts EVERY example's include/ on one path.

        _windows.yml says so in a comment -- "no two share a header name" -- and
        nothing checked it. The day a second example brings its own game.h, the
        first include/ on the list wins for both, and one of them is compiled
        against the other's header: a syntax pass that fails for no reason in
        the file it names, or passes against the wrong declarations.
        """
        seen = {}
        for header in sorted((REPO / "examples").glob("**/include/**/*.h")):
            rel = header.relative_to(REPO)
            # The path below include/, which is what `#include "..."` names.
            parts = rel.parts
            name = "/".join(parts[parts.index("include") + 1:])
            with self.subTest(header=str(rel)):
                self.assertNotIn(name, seen,
                                 f"{rel} and {seen.get(name)} are both \"{name}\": the MSVC "
                                 "syntax pass in _windows.yml puts every example's include/ "
                                 "on one path, so rename one of them")
            seen.setdefault(name, rel)
        self.assertIn("game.h", seen)  # or the glob found nothing and proved nothing


# ---------------------------------------------------------------------------
# The Android JNI boundary
# ---------------------------------------------------------------------------

RAYMOB_C = sorted((REPO / "thirdparty" / "raymob").glob("*.c"))
PROGUARD = REPO / "raymob" / "app" / "proguard-rules.pro"

# A JNI lookup is a STRING, on both sides of the boundary, and nothing checks
# it at build time. That is what makes the two gates below worth having.
JNI_LOOKUP = re.compile(r"\bGet(?:Static)?(?:Method|Field)ID\s*\(")
JNI_CALL = re.compile(r"\bCall(?:Static|Nonvirtual)?[A-Za-z]*Method\s*\(")


def code_lines(text, count):
    """The first `count` lines of `text` that are neither blank nor comment.

    Counting raw lines would make the gate fail the day somebody writes a
    longer comment above the check, which is the wrong thing to punish.
    """
    out = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("//", "/*", "*")):
            continue
        out.append(stripped)
        if len(out) == count:
            break
    return out


def proguard_keeps(text, cls):
    """Does this ProGuard text keep `cls` AND all of its members?

    `-keep class X { public <methods>; }` keeps methods and NOT fields, which
    is the hole this exists to find, so only an all-members spec counts.
    """
    for pattern, members in re.findall(r"-keep\s+class\s+([\w.$*]+)\s*\{([^}]*)\}", text):
        if members.strip() not in ("*;", "*"):
            continue
        rx = re.escape(pattern).replace(r"\*\*", ".*").replace(r"\*", "[^.]*")
        if re.fullmatch(rx, cls):
            return True
    return False


class AndroidProguardKeepTest(unittest.TestCase):
    """R8 renames what it is not told to keep, and JNI looks up by name.

    `minifyEnabled true` is on for release (raymob/app/build.gradle) and the
    Play upload is `./gradlew bundleRelease`. A renamed field is not a build
    error: it is GetFieldID returning NULL with a NoSuchFieldError pending, and
    ART killing the process at the next JNI call. Debug is not minified, so the
    emulator smoke test in CI cannot see any of it -- this test is the only
    thing between a new JNI lookup and a crash that happens only on Play.
    """

    def jni_classes(self):
        """Every com.raylib.raymob class the native side names, plus every one
        that exists. Both halves matter: the C names `Features`, which has no
        .java file, and the Java side has AdmobBridge, which the C reaches
        through NativeLoader rather than by name."""
        found = set()
        for path in RAYMOB_C:
            found.update(re.findall(r"com/raylib/raymob/(\w+)", path.read_text()))
        for path in (REPO / "raymob" / "app" / "src").rglob("java/com/raylib/raymob/*.java"):
            found.add(path.stem)
        return sorted("com.raylib.raymob." + name for name in found)

    def test_the_class_list_is_not_empty(self):
        """If the scrape ever finds nothing, the test below passes vacuously."""
        classes = self.jni_classes()
        for expected in ("NativeLoader", "DisplayManager", "SoftKeyboard"):
            self.assertIn("com.raylib.raymob." + expected, classes)
        self.assertGreaterEqual(len(classes), 4, classes)

    def test_every_class_the_jni_names_is_kept_with_all_its_members(self):
        text = PROGUARD.read_text()
        for cls in self.jni_classes():
            with self.subTest(cls=cls):
                self.assertTrue(proguard_keeps(text, cls),
                                cls + " has no keep-all rule in proguard-rules.pro -- "
                                "R8 will rename its fields and methods in the release "
                                "AAB and the JNI lookups for them return NULL")

    def test_the_rule_this_file_used_to_carry_would_not_pass(self):
        """Proof that the gate is red for the thing it was written for.

        This is the rule that shipped: NativeLoader's public METHODS, which
        left the `initCallback`, `displayManager` and `softKeyboard` fields and
        the other two classes to be renamed.
        """
        old = "-keep class com.raylib.raymob.NativeLoader {\n    public <methods>;\n}"
        self.assertFalse(proguard_keeps(old, "com.raylib.raymob.NativeLoader"))
        self.assertFalse(proguard_keeps(old, "com.raylib.raymob.DisplayManager"))
        # And the rule that replaced it does cover them.
        new = "-keep class com.raylib.raymob.** { *; }"
        self.assertTrue(proguard_keeps(new, "com.raylib.raymob.NativeLoader"))
        self.assertTrue(proguard_keeps(new, "com.raylib.raymob.DisplayManager"))

    def test_the_package_is_spelled_the_way_gradle_rewrites_it(self):
        """build.gradle does a literal substitution of `com.raylib.raymob` in
        this file for the real application id before a build, and back after.
        A rule spelled any other way silently stops applying."""
        for line in PROGUARD.read_text().splitlines():
            if line.strip().startswith("-keep") and "raymob" in line:
                self.assertIn("com.raylib.raymob", line,
                              "a keep rule naming the package must spell it "
                              "com.raylib.raymob, which is what build.gradle rewrites")

    def test_native_methods_are_kept_without_relying_on_the_default_file(self):
        """The four onApp* callbacks are registered from C with RegisterNatives
        and are private, so nothing in Java references them."""
        self.assertRegex(PROGUARD.read_text(),
                         r"-keepclasseswithmembernames\s+class\s+\*\s*\{\s*native\s+<methods>;")


    def test_nothing_in_the_file_is_mangled_by_the_regex_rewrite(self):
        """build.gradle rewrites `com.raylib.raymob` in proguard-rules.pro with
        String.replaceAll, which is a REGEX: the dots match any character. A
        comment that spelt the package `com/raylib/raymob` was rewritten to
        the app id and restored as `com.raylib.raymob`, and the Android job's
        "working tree clean" step went red on a comment. Every regex match has
        to be the literal, so the rewrite and its restore are inverses."""
        text = (REPO / "raymob" / "app" / "proguard-rules.pro").read_text()
        for found in re.findall(r"com.raylib.raymob", text):
            self.assertEqual(found, "com.raylib.raymob",
                             f"{found!r} matches the rewrite pattern and is not the literal")

class RaymobJniDisciplineTest(unittest.TestCase):
    """Two mistakes in JNI code are aborts, not failures, and both were here.

    A jfieldID of NULL handed to GetObjectField is `JNI DETECTED ERROR IN
    APPLICATION` and the process dies. A pending Java exception is worse: it
    stays on the thread and kills the VM at the next JNI call made from ANY
    file, so the crash is reported against code that did nothing wrong.

    None of it is visible on this machine -- the whole directory is inside
    `#ifdef __ANDROID__` -- so the gate is a read of the source. Each failure
    is describable precisely, which by Omar's rule means something automated
    should reject it rather than the next reader having to remember.
    """

    def test_there_are_jni_call_sites_to_check(self):
        """A regex that stops matching would make every test below vacuous."""
        lookups = sum(len(JNI_LOOKUP.findall(p.read_text())) for p in RAYMOB_C)
        calls = sum(len(JNI_CALL.findall(p.read_text())) for p in RAYMOB_C)
        self.assertGreater(lookups, 0, "no GetMethodID/GetFieldID found at all")
        self.assertGreater(calls, 10, "no Call*Method found at all")

    def test_every_lookup_is_null_checked(self):
        for path in RAYMOB_C:
            text = path.read_text()
            for match in JNI_LOOKUP.finditer(text):
                # The statement, not the line: an assignment can be wrapped.
                head = text[:match.start()]
                start = max(head.rfind(";"), head.rfind("{"), head.rfind("}"))
                names = re.findall(r"(\w+)\s*=", head[start + 1:])
                end = text.find(";", match.end())
                line = text.count("\n", 0, match.start()) + 1
                with self.subTest(site=path.name + ":" + str(line)):
                    self.assertTrue(names, "the lookup result is not assigned to anything")
                    var = names[-1]
                    tail = " ".join(code_lines(text[end + 1:], 6))
                    self.assertRegex(
                        tail, r"\b" + re.escape(var) + r"\s*(?:==|!=)\s*NULL",
                        var + " is not checked against NULL within six lines -- "
                        "GetMethodID/GetFieldID returns NULL *and* leaves a pending "
                        "exception when the member is missing, which R8 makes real "
                        "in the release AAB")

    def test_every_call_is_followed_by_an_exception_check(self):
        for path in RAYMOB_C:
            text = path.read_text()
            for match in JNI_CALL.finditer(text):
                end = text.find(";", match.end())
                line = text.count("\n", 0, match.start()) + 1
                with self.subTest(site=path.name + ":" + str(line)):
                    tail = " ".join(code_lines(text[end + 1:], 6))
                    self.assertIn(
                        "ExceptionCheck", tail,
                        "no ExceptionCheck within six lines of this Call*Method -- "
                        "a Java-side throw stays pending and aborts the VM at the "
                        "next JNI call from anywhere")

    def test_the_detach_is_conditional_on_having_attached(self):
        """raylib's game loop thread is attached for the life of the process,
        and detaching it invalidates every local reference the caller holds.
        Only a thread this code attached may be detached."""
        helper = (REPO / "thirdparty" / "raymob" / "helper.c").read_text()

        attach = helper.split("JNIEnv* AttachCurrentThread(void)")[1].split("\n}")[0]
        self.assertIn("GetEnv", attach,
                      "AttachCurrentThread must ask GetEnv whether the thread is "
                      "already attached before it attaches anything")

        # What matters is the code BEFORE the detach call: a mention of the
        # flag anywhere in the body is satisfied by the assignment that follows
        # the call, which is how this test first passed on an unguarded detach.
        detach = helper.split("void DetachCurrentThread(void)")[1].split("\n}")[0]
        before = detach.split("DetachCurrentThread(vm)")[0]
        self.assertIn("attachedHere", before,
                      "DetachCurrentThread detaches without first asking whether "
                      "this code is what attached the thread")
        self.assertIn("return", before,
                      "nothing bails out before the detach, so the guard cannot "
                      "be doing anything")

    def test_the_cache_path_allocation_has_room_for_the_separator(self):
        r"""dir + '/' + name + '\0'. Upstream allocated one byte less and wrote
        the terminator past the end of the block -- an immediate abort under
        scudo, which is Android's allocator since API 30."""
        helper = (REPO / "thirdparty" / "raymob" / "helper.c").read_text()
        body = helper.split("char* LoadCacheFile")[1].split("\n}")[0]
        # Code only: the comment at the site quotes the line it replaced, and a
        # gate that reads prose is a gate that fires on its own explanation.
        code = " ".join(code_lines(body, 400))
        self.assertIn("strlen(cacheDir) + 1 + strlen(fileName) + 1", code)
        self.assertNotIn("filePath[len]", code,
                         "writing at filePath[len] is one past the end of a len-byte block")

    def test_the_orientation_bound_check_reads_the_value_it_guards(self):
        """`if (result >= 0 && result < 4)` where result is still 0 is
        vacuously true, so the check never saw the value it was written for."""
        display = (REPO / "thirdparty" / "raymob" / "display.c").read_text()
        body = " ".join(code_lines(display.split("Orientation GetScreenOrientation")[1], 400))
        self.assertIn("screenOrientation >= 0 && screenOrientation < 4", body)
        self.assertNotIn("result >= 0 && result < 4", body)

    def test_on_key_up_delegates_to_on_key_up(self):
        """The override for key-up returned super.onKeyDown, so releasing the
        hardware BACK button sent a second down and never an up."""
        java = (REPO / "raymob" / "app" / "src" / "main" / "java" / "com" / "raylib"
                / "raymob" / "NativeLoader.java").read_text()
        body = java.split("public boolean onKeyUp")[1].split("\n    }")[0]
        self.assertIn("super.onKeyUp(", body)
        self.assertNotIn("super.onKeyDown(", body)


class ZeroEntryPackFixtureTest(unittest.TestCase):
    """tests/fixtures/pack_zero/resources.rres is generated, not hand-carved.

    It is 96 bytes of binary and it exists for one assertion: open_pack() has
    to refuse a pack whose central directory declares no entries, and give the
    directory rres allocated back. A binary fixture with no generator beside it
    is a blob nobody dares change, and a generator that has drifted from the
    committed bytes is worse than none -- so the bytes are compared, not the
    intention.
    """

    FIXTURE = REPO / "tests" / "fixtures" / "pack_zero" / "resources.rres"
    SCRIPT = REPO / "tools" / "make_zero_pack.py"

    def test_the_script_writes_exactly_the_committed_bytes(self):
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "resources.rres"
            run = subprocess.run([sys.executable, str(self.SCRIPT), str(out)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(
                out.read_bytes(), self.FIXTURE.read_bytes(),
                "tools/make_zero_pack.py no longer writes the committed fixture; "
                "run it to regenerate tests/fixtures/pack_zero/resources.rres")

    def test_the_central_directory_is_present_and_declares_no_entries(self):
        """The two things that make the fixture the case under test.

        A cdOffset of 0 is rres's "no central directory at all", answered
        without allocating -- the path that never leaked. And rres reads the
        entry count out of props[0] without checking propCount, so a directory
        with no properties is a null dereference inside rres rather than an
        empty pack.
        """
        import struct
        data = self.FIXTURE.read_bytes()
        self.assertEqual(data[:4], b"rres")
        self.assertEqual(struct.unpack_from("<H", data, 4)[0], 100, "file version")

        cd_offset = struct.unpack_from("<I", data, 8)[0]
        self.assertNotEqual(cd_offset, 0,
                            "cdOffset 0 means 'no central directory' and rres "
                            "allocates nothing on that path")
        info_at = 16 + cd_offset  # rres seeks from the end of the header
        self.assertEqual(data[info_at:info_at + 4], b"CDIR")

        prop_count, entries = struct.unpack_from("<II", data, info_at + 32)
        self.assertEqual(prop_count, 1, "rres reads props[0] without checking this")
        self.assertEqual(entries, 0, "the whole point: a directory holding nothing")


# A C file linked into the packer by the allocation test below: every
# malloc/calloc in tools/rres_pack.c is renamed to one of these, and the Nth
# call -- N from RMP_FAIL_ALLOC -- returns NULL, the way a machine out of memory
# answers.
FAULT_SHIM = r"""
#include <stdlib.h>
static long rmp_calls = 0;
static int rmp_fails_now(void) {
    const char *at = getenv("RMP_FAIL_ALLOC");
    return at != NULL && ++rmp_calls == atol(at);
}
void *rmp_fault_malloc(size_t size) { return rmp_fails_now() ? NULL : malloc(size); }
void *rmp_fault_calloc(size_t count, size_t size) {
    return rmp_fails_now() ? NULL : calloc(count, size);
}
"""


class RresPackTest(unittest.TestCase):
    """tools/rres_pack.c, the packer `pack_resources` runs for every desktop
    release, built from the sources CMakeLists.txt gives it and run here.

    It had no test. gcc's analyzer found two allocations used without a check
    -- the entry table and each chunk's plaintext, a NULL dereference the day
    a pack is bigger than the memory left -- and every other allocation in it
    was unchecked as well. So each one is made to fail in turn, and the packer
    has to say so and exit 1: never a crash, never a pack."""

    @classmethod
    def setUpClass(cls):
        import shutil
        cls.cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    @staticmethod
    def sources() -> list[Path]:
        """What CMakeLists.txt compiles into rres_pack, read from it, a list
        variable expanded where it names one."""
        text = (REPO / "CMakeLists.txt").read_text()
        found = re.search(r"add_executable\(rres_pack\s+([^)]*)\)", text)
        out = []
        for word in found.group(1).split():
            named = re.fullmatch(r"\$\{(\w+)\}", word)
            if named:
                listed = re.search(rf"set\({named.group(1)}\s+([^)]*)\)", text)
                out += listed.group(1).split()
            else:
                out.append(word)
        return [REPO / s.strip('"').replace("${CMAKE_CURRENT_SOURCE_DIR}/", "") for s in out]

    def build(self, name: str, fault: bool = False) -> Path:
        import subprocess
        if self.cc is None or sys.platform == "win32":
            if IN_BUILD_IMAGE:
                self.fail("the build image has a C compiler; this test cannot skip there")
            self.skipTest("no C compiler here")
        out = self.tmp / name
        if out.exists():
            return out
        flags = ["-O0", "-g", f"-I{REPO / 'tools'}", f"-I{REPO / 'thirdparty' / 'rres'}"]
        objects = []
        for i, source in enumerate(self.sources()):
            extra = []
            if fault and source.name == "rres_pack.c":
                extra = ["-Dmalloc=rmp_fault_malloc", "-Dcalloc=rmp_fault_calloc"]
            obj = self.tmp / f"{name}_{i}.o"
            got = subprocess.run([self.cc, *flags, *extra, "-c", str(source), "-o", str(obj)],
                                 capture_output=True, text=True)
            self.assertEqual(got.returncode, 0, got.stderr[-2000:])
            objects.append(obj)
        if fault:
            shim = self.tmp / "fault_shim.c"
            shim.write_text(FAULT_SHIM)
            objects.append(shim)
        got = subprocess.run([self.cc, *map(str, objects), "-o", str(out)],
                             capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stderr[-2000:])
        return out

    def inputs(self) -> list[Path]:
        folder = self.tmp / "in"
        folder.mkdir(exist_ok=True)
        empty, text = folder / "empty.txt", folder / "hello.json"
        empty.write_bytes(b"")
        text.write_bytes(b'{"hello": "world"}\n')
        return [empty, text]

    def pack(self, binary: Path, password: str, env=None):
        import subprocess
        out = self.tmp / "out.rres"
        if out.exists():
            out.unlink()
        got = subprocess.run([str(binary), str(out), password, *map(str, self.inputs())],
                             capture_output=True, text=True, env=env, stdin=subprocess.DEVNULL)
        return got, out

    @staticmethod
    def plaintext(source: Path) -> bytes:
        """A raw chunk before encryption: propCount 4, the size, the extension
        in two big-endian words, a reserved 0, then the bytes."""
        import struct
        raw, ext = source.read_bytes(), source.suffix.encode()

        def word(start):
            value = 0
            for i in range(start, start + 4):
                value = (value << 8) | (ext[i] if i < len(ext) else 0)
            return value
        return struct.pack("<IIIII", 4, len(raw), word(0), word(4), 0) + raw

    def chunks(self, data: bytes) -> list[dict]:
        import struct
        import zlib
        self.assertEqual(data[:4], b"rres")
        count = struct.unpack_from("<H", data, 6)[0]
        at, found = 16, []
        for _ in range(count):
            kind = data[at:at + 4]
            cipher = data[at + 9]
            packed_size, base_size = struct.unpack_from("<II", data, at + 12)
            crc = struct.unpack_from("<I", data, at + 28)[0]
            packed = data[at + 32:at + 32 + packed_size]
            self.assertEqual(zlib.crc32(packed), crc, f"the CRC of chunk {len(found)}")
            found.append({"kind": kind, "cipher": cipher, "packed": packed,
                          "base_size": base_size})
            at += 32 + packed_size
        self.assertEqual(at, len(data), "the chunks account for the whole file")
        return found

    def test_it_packs_an_empty_file_and_a_small_one(self):
        """The empty file is the path gcc's analyzer could not follow: read_file()
        hands back a one-byte buffer and a size of 0."""
        got, out = self.pack(self.build("rres_pack"), "-")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        chunks = self.chunks(out.read_bytes())
        self.assertEqual([c["kind"] for c in chunks], [b"RAWD", b"RAWD", b"CDIR"])
        for chunk, source in zip(chunks, self.inputs()):
            with self.subTest(file=source.name):
                self.assertEqual(chunk["cipher"], 0, "RRES_CIPHER_NONE")
                self.assertEqual(chunk["packed"], self.plaintext(source))

    def test_an_encrypted_chunk_ends_in_the_md5_of_its_plaintext(self):
        import hashlib as md5_lib
        got, out = self.pack(self.build("rres_pack"), "a password")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        chunks = self.chunks(out.read_bytes())
        for chunk, source in zip(chunks, self.inputs()):
            with self.subTest(file=source.name):
                plain = self.plaintext(source)
                self.assertEqual(chunk["cipher"], 30, "RRES_CIPHER_AES")
                self.assertEqual(chunk["base_size"], len(plain))
                self.assertEqual(len(chunk["packed"]), len(plain) + 32)
                self.assertNotEqual(chunk["packed"][:len(plain)], plain, "not encrypted")
                self.assertEqual(chunk["packed"][-16:], md5_lib.md5(plain).digest(),
                                 "tools/md5.c is not MD5")

    def test_a_full_disk_is_an_error_and_not_a_pack(self):
        """The writes were never checked, and a buffered stream only finds the
        disk full at fclose(): the packer said "Packed" and exited 0 with the
        pack cut short. /dev/full is that disk, on Linux."""
        import subprocess
        if not Path("/dev/full").exists():
            self.skipTest("no /dev/full here")
        got = subprocess.run([str(self.build("rres_pack")), "/dev/full", "-",
                              *map(str, self.inputs())],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("cannot write", got.stderr)
        self.assertNotIn("Packed", got.stdout)

    def test_every_allocation_that_fails_is_an_error_and_never_a_crash(self):
        binary = self.build("rres_pack_fault", fault=True)
        for password in ("-", "a password"):
            failed = 0
            for n in range(1, 64):
                env = dict(os.environ, RMP_FAIL_ALLOC=str(n))
                got, out = self.pack(binary, password, env=env)
                if got.returncode == 0 and "Packed" in got.stdout:
                    break  # the Nth allocation never happened: all were tried
                failed += 1
                with self.subTest(password=password, failing_allocation=n):
                    self.assertEqual(got.returncode, 1,
                                     f"allocation {n} failing ended in "
                                     f"{got.returncode}, not exit 1:\n{got.stderr[-500:]}")
                    self.assertIn("out of memory", got.stderr)
                    self.assertFalse(out.exists(), "a pack was written anyway")
            else:
                self.fail("more than 63 allocations, or the packer never finished")
            self.assertGreaterEqual(failed, 4, f"only {failed} allocations with {password!r}")
# ---------------------------------------------------------------------------


def validate_rejections(source: str) -> list[tuple[int, str]]:
    """(line, key phrase) for every `raise ConfigError` lexically in validate().

    Parsed, not grepped, so a `raise` inside a nested `if` or a loop counts the
    same as one at the top, and a ConfigError raised from somewhere else in the
    file does not. The KEY PHRASE is what links a rejection to its test: the
    `[section] key` the message opens with, or -- when that is interpolated,
    as in `f"[android.{table}] {k} = ..."` -- the first literal sentence long
    enough to be distinctive.
    """
    fn = next(n for n in ast.parse(source).body
              if isinstance(n, ast.FunctionDef) and n.name == "validate")
    hole = "\x00"

    def literal(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if isinstance(node, ast.JoinedStr):
            return "".join(v.value if isinstance(v, ast.Constant)
                           and isinstance(v.value, str) else hole for v in node.values)
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            return literal(node.left) + literal(node.right)
        return ""

    key_re = re.compile(r"\[[a-z][a-z.]*\] [a-z_]+\b")
    out = []
    for node in ast.walk(fn):
        if not (isinstance(node, ast.Raise) and isinstance(node.exc, ast.Call)):
            continue
        func = node.exc.func
        if getattr(func, "id", getattr(func, "attr", "")) != "ConfigError":
            continue
        if not node.exc.args:
            continue
        text = literal(node.exc.args[0])
        found = key_re.search(text)
        if found:
            out.append((node.lineno, found.group(0)))
            continue
        phrase = ""
        for frag in re.split(r"[\x00\n]", text):
            frag = re.sub(r"\s+", " ", frag).strip(" .,:;=")
            if len(frag) >= 18:
                phrase = frag[:60].rsplit(" ", 1)[0] if len(frag) > 60 else frag
                break
        out.append((node.lineno, phrase or re.sub(r"\s+", " ", text.replace(hole, " ")).strip()))
    return out


class ConfigureRejectionCoverageTest(unittest.TestCase):
    """CLAUDE.md: "every combination that cannot work is a rejection with a
    reason, and every rejection has a test in tests/configure_test.py".

    That was an intention, not a fact. About a dozen rejections had never been
    fired by anything -- [ui] scale, [input] deadzone, [android] min_sdk, the
    com.raylib.raymob rule, the firebase charsets -- and the existing
    "every rejection can be located" test only checked that an error could be
    POINTED AT, never that it happens.

    So this is the meta-gate: it parses validate(), extracts the key phrase of
    every `raise ConfigError`, and asserts each phrase is used as an expectation
    somewhere in this file. It cannot be satisfied by a test that merely calls
    validate() -- the phrase only appears if something asserts on the message.
    The table in ConfigureEveryRejectionFiresTest below is where the missing
    ones went.
    """

    @staticmethod
    def expectations():
        """Every string this file USES -- in an assertion, a CASES key, a call
        -- and none of the ones it only SAYS: docstrings and comments are left
        out. The first version searched the raw text, so a docstring that
        mentioned "[window] vsync" satisfied the gate with no test behind it
        at all."""
        tree = ast.parse(Path(__file__).read_text())
        docstrings = set()
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
                                 ast.AsyncFunctionDef)) and node.body:
                first = node.body[0]
                if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) \
                        and isinstance(first.value.value, str):
                    docstrings.add(id(first.value))
        return "\n".join(n.value for n in ast.walk(tree)
                         if isinstance(n, ast.Constant) and isinstance(n.value, str)
                         and id(n) not in docstrings)

    def test_every_rejection_in_validate_has_a_test_that_names_it(self):
        source = (REPO / "tools" / "configure.py").read_text()
        mine = self.expectations()
        phrases = validate_rejections(source)
        self.assertGreater(len(phrases), 40,
                           "the parse found almost no rejections, which looks exactly "
                           "like a validate() with nothing to check")
        missing = sorted({p for _, p in phrases if p not in mine})
        self.assertEqual(
            missing, [],
            "these rejections in validate() have no test that expects their message:\n  "
            + "\n  ".join(missing)
            + "\n\nAdd a row to ConfigureEveryRejectionFiresTest.CASES with a config that "
              "triggers it, using the phrase as the needle.")


class ConfigureEveryRejectionFiresTest(unittest.TestCase):
    """One config per rejection, and the rejection actually happens.

    The table is the point: it is what the meta-gate above counts. Each row is
    a mutation of a valid config that can only be answered by the message named
    in the key -- so a rejection that gets deleted, renamed or accidentally
    made unreachable fails here rather than going quiet.
    """

    # phrase -> (section, key-path, value). The path is a tuple so nested
    # tables ([android.admob] enabled) fit the same shape as flat ones.
    CASES = {
        "has to be a number between 0 and 1": ("audio", ("music",), "loud"),
        "[save] portable": ("save", ("portable",), "yes"),
        "[save] encrypt": ("save", ("encrypt",), 1),
        "[save] version": ("save", ("version",), 0),
        "has to be between 0 and 1":  ("audio", ("sfx",), 2),
        "[linux] wayland":            ("linux", ("wayland",), "yes"),
        "[web] memory":               ("web", ("memory",), 8),
        "[web] grow":                 ("web", ("grow",), "true"),
        "[project] name":             ("project", ("name",), "9lives"),
        "[window] title":             ("window", ("title",), "two\nlines"),
        "[window] title is empty":    ("window", ("title",), "  "),
        "[icon] source is empty":     ("icon", ("source",), ""),
        "[window] orientation":       ("window", ("orientation",), "sideways"),
        "[window] vsync":             ("window", ("vsync",), "yes"),
        "[app] max_delta":            ("app", ("max_delta",), "fast"),
        "[deploy] licenses":          ("deploy", ("licenses",), "yes"),
        "[dev] compiler":             ("dev", ("compiler",), "icc"),
        "[dev] strict":               ("dev", ("strict",), "yes"),
        "[ci] on_push":               ("ci", ("on_push",), "false"),
        "[android] gradle_offline":   ("android", ("gradle_offline",), "no"),
        "[linux] glibc":              ("linux", ("glibc",), 2.28),
        "[upx] max_size_mb":          ("upx", ("max_size_mb",), "big"),
        "cannot be compressed \u2014":  ("upx", ("enabled",), ["macos"]),
        "unknown target or group":    ("upx", ("disabled",), ["playstation"]),
        "[web] backend":              ("web", ("backend",), "sdl"),
        "[window] fps":               ("window", ("fps",), -1),
        "must be an integer between 16 and 16384": ("window", ("width",), 4),
        "[android] category":         ("android", ("category",), "not-a-category"),
        "must be true or false":      ("android", ("display", "keep_on"), "false"),
        "[android.admob] enabled":    ("android", ("admob", "enabled"), "yes"),
        "[android] application_id":   ("android", ("application_id",), "nodotshere"),
        "[ios] bundle_id":            ("ios", ("bundle_id",), "nodotshere"),
        "[ios] deployment_target":    ("ios", ("deployment_target",), "fifteen"),
        "[android] gl_version":       ("android", ("gl_version",), "ES10"),
        "[android] min_sdk":          ("android", ("min_sdk",), 19),
        "[raylib] disabled_modules":  ("raylib", ("disabled_modules",), ["rcore"]),
        "[dev] linker":               ("dev", ("linker",), "gold"),
        "[icon] adaptive_background": ("icon", ("adaptive_background",), "blue"),
        "[resources] rres_password":  ("resources", ("rres_password",), ""),
        "[deploy] credits_note":      ("deploy", ("credits_note",), 5),
        'or "" to leave it unset':    ("deploy", ("itch", "user"), "not a slug!"),
        "[input] deadzone":           ("input", ("deadzone",), 0.99),
        "[ui] font":                  ("ui", ("font",), 12),
        "[ui] font_size":             ("ui", ("font_size",), 2),
        "[ui] scale":                 ("ui", ("scale",), 99),
        "[ui] max_elements":          ("ui", ("max_elements",), 4),
    }

    def build(self, section, path, value):
        table = copy.deepcopy(cfgmod.DEFAULTS[section])
        node = table
        for step in path[:-1]:
            node = node[step]
        node[path[-1]] = value
        cfg = base_config(**{section: table})
        # base_config() makes the two example ids real; a row that overwrites
        # one of them on purpose keeps what it asked for.
        return cfg

    def test_each_rejection_fires_with_its_own_message(self):
        for phrase, (section, path, value) in self.CASES.items():
            with self.subTest(rejection=phrase):
                cfg = self.build(section, path, value)
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(cfg, False)
                self.assertIn(phrase, str(caught.exception))

    def test_the_second_application_id_rule_is_the_raymob_one(self):
        """A valid id that CONTAINS com.raylib.raymob. Gradle string-substitutes
        that exact text into the raymob sources and reverts it afterwards, so an
        id containing it makes the revert non-idempotent and mangles them."""
        cfg = base_config()
        cfg["android"]["application_id"] = "com.raylib.raymob.mine"
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        self.assertIn("com.raylib.raymob", str(caught.exception))

    def test_an_unknown_raylib_module_is_a_different_message(self):
        cfg = base_config()
        cfg["raylib"]["disabled_modules"] = ["rnotathing"]
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        self.assertIn("unknown module", str(caught.exception))

    def test_placeholder_identifiers_are_refused_on_a_release(self):
        """refusing to build a release with placeholder identifiers -- the one
        rejection whose consequence cannot be taken back, because a Google Play
        application id is permanent."""
        cfg = copy.deepcopy(cfgmod.DEFAULTS)     # NOT base_config(): the examples
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, True)
        self.assertIn("refusing to build a release with placeholder identifiers",
                      str(caught.exception))

    def test_every_com_example_id_is_a_placeholder_on_a_release(self):
        """`rmp new` writes com.example.<name>. Only the exact
        com.example.raytest was refused, so com.example.my_game sailed through
        --strict-release: the one id that can never be changed once published."""
        for appid in ("com.example.my_game", "com.example.a", "com.example.x.y"):
            with self.subTest(id=appid):
                cfg = base_config(android=dict(copy.deepcopy(cfgmod.DEFAULTS["android"]),
                                               application_id=appid))
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(cfg, True)
                self.assertIn(appid, str(caught.exception))
        for appid in ("com.examples.game", "org.example.game", "com.omardev.example"):
            with self.subTest(id=appid):
                cfg = base_config(android=dict(copy.deepcopy(cfgmod.DEFAULTS["android"]),
                                               application_id=appid))
                with quiet():
                    cfgmod.validate(cfg, True)


class RenderGoldenTest(unittest.TestCase):
    """The framework's golden frame exists, and render_check.sh compares
    against it whenever it is there.

    A game has no golden -- its title screen is its author's to change -- so
    render_check.sh passes on the boot, the pixels and a clean exit when the
    file is absent, in CI too. That branch used to fail in CI precisely so a
    deleted fixture could not go green; this test is what says it in the
    framework instead."""

    GOLDEN = REPO / "tests" / "fixtures" / "render_hash.txt"

    def test_the_framework_golden_is_there_and_is_a_hash(self):
        self.assertTrue(self.GOLDEN.is_file(), "the framework's golden frame hash is gone")
        self.assertRegex(self.GOLDEN.read_text(), r"^[0-9a-f]{8}\n?$")

    def test_render_check_compares_when_it_exists_and_passes_without(self):
        text = (REPO / "tools" / "render_check.sh").read_text()
        self.assertIn('if [ ! -f "$GOLDEN" ]; then', text)
        self.assertIn('test "$GOT" = "$WANT"', text)
        self.assertIn("no golden hash: checked the boot, the pixels and a clean exit", text)

    def test_the_musl_job_compares_only_when_it_exists(self):
        text = (REPO / ".github" / "workflows" / "_linux.yml").read_text()
        self.assertIn("if [ -f tests/fixtures/render_hash.txt ]; then", text)


class GameResourcesTest(unittest.TestCase):
    """cmake/game_resources.cmake: an example reads, and a web build preloads,
    its OWN resources/ -- in a production build too.

    Production used to preload the project's resources/ for every target, so
    an example built for the web in production -- which is how the docs site
    plays them -- shipped the project's art and failed to load its own."""

    @classmethod
    def setUpClass(cls):
        import shutil
        cls.cmake = shutil.which("cmake")

    def resolve(self, production, has_own):
        import subprocess
        import tempfile
        if self.cmake is None:
            self.skipTest("cmake not installed")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "project"
            (root / "resources").mkdir(parents=True)
            game = root / "examples" / "games" / "demo"
            game.mkdir(parents=True)
            if has_own:
                (game / "resources").mkdir()
            script = Path(tmp) / "probe.cmake"
            script.write_text(
                f'set(CMAKE_CURRENT_SOURCE_DIR "{root.as_posix()}")\n'
                f'set(PRODUCTION_BUILD {"ON" if production else "OFF"})\n'
                'set(RESOURCES_PATH "/fallback/")\n'
                f'include("{(REPO / "cmake" / "game_resources.cmake").as_posix()}")\n'
                f'rmp_game_resources("{game.as_posix()}" P D)\n'
                'message(STATUS "PATH=${P}|DIR=${D}")\n')
            got = subprocess.run([self.cmake, "-P", str(script)], capture_output=True, text=True)
            self.assertEqual(got.returncode, 0, got.stderr)
            m = re.search(r"PATH=(.*)\|DIR=(.*)", got.stdout + got.stderr)
            return (m.group(1).strip(), m.group(2).strip().replace(root.as_posix(), "<root>"))

    def test_debug_reads_the_folder_it_has(self):
        self.assertEqual(self.resolve(False, True)[1], "<root>/examples/games/demo/resources")
        self.assertTrue(self.resolve(False, True)[0].endswith("/examples/games/demo/resources/"))
        self.assertEqual(self.resolve(False, False), ("/fallback/", "<root>/resources"))

    def test_production_preloads_the_games_own_folder(self):
        self.assertEqual(self.resolve(True, True), ("./resources/", "<root>/examples/games/demo/resources"))
        self.assertEqual(self.resolve(True, False), ("./resources/", "<root>/resources"))

    def test_cmakelists_uses_it(self):
        text = (REPO / "CMakeLists.txt").read_text()
        self.assertIn("include(cmake/game_resources.cmake)", text)
        self.assertIn('rmp_game_resources("${DIR}" _res _res_dir)', text)


class ConfigTablesTest(unittest.TestCase):
    """What configure.py prints for the documentation is what it does.

    --print-defines is the table gen_app_config() writes the header FROM, so a
    define cannot exist without its row; --print-schema's choices are checked
    against validate() value by value, so the table cannot offer a value that
    is refused or leave out one that is taken; every key of DEFAULTS has its
    comment in the .toml, which is what the site shows for it."""

    def run_cfg(self, *argv, cwd=None):
        import subprocess
        import tempfile  # noqa: F401 - used by the callers below
        return subprocess.run([sys.executable, str(REPO / "tools" / "configure.py"), *argv],
                              capture_output=True, text=True, cwd=cwd or REPO)

    def test_every_define_in_the_header_is_a_row_and_back(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            cfg = cfgmod.load_config()
            original = cfgmod.REPO
            try:
                cfgmod.REPO = Path(tmp)
                cfgmod.gen_app_config(cfg)
                header = (Path(tmp) / "include" / "rmp" / "generated" / "config.h").read_text()
            finally:
                cfgmod.REPO = original
        written = re.findall(r"^#define (RMP_\w+) (.*)$", header, re.M)
        written = [(n, v.strip()) for n, v in written if n != "RMP_GENERATED_CONFIG_H"]
        rows = [(d["name"], d["value"]) for d in cfgmod.app_defines(cfg)]
        self.assertEqual(written, rows)
        self.assertGreater(len(rows), 20)
        got = self.run_cfg("--print-defines")
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertEqual([(d["name"], d["value"]) for d in json.loads(got.stdout)], rows)

    def test_every_row_names_a_key_of_the_toml(self):
        for entry in cfgmod.APP_DEFINES:
            if isinstance(entry, str):
                continue
            name, key, _render = entry
            with self.subTest(define=name):
                section, field = key.split(".", 1)
                self.assertIn(field, cfgmod.DEFAULTS[section])

    def test_every_key_has_its_comment_in_the_toml(self):
        rows = cfgmod.schema()
        self.assertGreater(len(rows), 50)
        for row in rows:
            with self.subTest(key=row["key"]):
                self.assertTrue(row["comment"].strip(), f"{row['key']} has no comment in "
                                "raylib_multiplatform.toml; the site shows that comment for it")

    def validates(self, key, value):
        cfg = copy.deepcopy(cfgmod.DEFAULTS)   # DEFAULTS itself must never change
        table = cfg
        *path, last = key.split(".")
        for part in path:
            table = table[part]
        table[last] = value
        try:
            # What a run does with the values before it writes anything:
            # [targets] and [upx] names are resolved after validate().
            cfgmod.validate(cfg, strict_release=False)
            targets = cfgmod.expand_targets(cfg["targets"]["enabled"], cfg["targets"]["disabled"])
            cfgmod.expand_upx(cfg, targets)
            return True
        except cfgmod.ConfigError:
            return False

    def test_every_choice_is_taken_and_nothing_else(self):
        for key, allowed in cfgmod.ALLOWED.items():
            for value in sorted(allowed):
                with self.subTest(key=key, value=value):
                    if key == "dev.compiler" and value in ("mingw", "msvc") and not cfgmod.on_windows():
                        continue    # Windows toolchains, refused elsewhere on purpose
                    self.assertTrue(self.validates(key, value), f"{key} = {value!r} is refused")
            with self.subTest(key=key, value="not-a-choice"):
                self.assertFalse(self.validates(key, "not-a-choice"))

    def test_every_list_item_is_taken_and_nothing_else(self):
        for key, allowed in cfgmod.ALLOWED_ITEMS.items():
            items = allowed()
            if key == "targets.disabled":
                items = [i for i in items if i != "all"]   # disabling everything is its own refusal
            for value in items:
                with self.subTest(key=key, value=value):
                    self.assertTrue(self.validates(key, [value]), f"{key} = [{value!r}] is refused")
            with self.subTest(key=key, value="not-a-choice"):
                self.assertFalse(self.validates(key, ["not-a-choice"]))

    def test_config_reads_another_toml_and_writes_nothing(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "good.toml"
            good.write_text('[window]\nwidth = 640\n')
            bad = Path(tmp) / "bad.toml"
            bad.write_text('[window]\nwidth = "wide"\n')
            got = self.run_cfg("--check", "--config", str(good))
            self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
            got = self.run_cfg("--check", "--config", str(bad))
            self.assertNotEqual(got.returncode, 0)
            self.assertIn("[window] width", got.stdout + got.stderr)
            got = self.run_cfg("--config", str(good))
            self.assertNotEqual(got.returncode, 0)
            self.assertIn("never generates", got.stdout + got.stderr)
            got = self.run_cfg("--check", "--config", str(Path(tmp) / "nope.toml"))
            self.assertNotEqual(got.returncode, 0)
            self.assertIn("no such file", got.stdout + got.stderr)
            got = self.run_cfg("--print-defines", "--config", str(good))
            width = next(d for d in json.loads(got.stdout) if d["name"] == "RMP_WINDOW_WIDTH")
            self.assertEqual(width["value"], "640")

    def test_the_targets_table_is_every_target(self):
        got = self.run_cfg("--print-targets-table")
        rows = json.loads(got.stdout)
        self.assertEqual([r["id"] for r in rows], list(cfgmod.TARGETS))
        for r in rows:
            self.assertIn("all", r["groups"])


class FindPythonTest(unittest.TestCase):
    """cmake/find_python.cmake asks every candidate, not just the first found.

    The hook took the first of python3/python/py on PATH and probed only that
    one, so a Windows machine whose `python3.exe` is the Microsoft Store stub
    -- an advert and exit code 9009 -- failed to configure with a working `py`
    installed, and a Mac's own 3.9 hid a Homebrew 3.12. Run with `cmake -P`
    against a PATH built here: a stub that fails, then a real Python."""

    def setUp(self):
        import shutil
        self.cmake = shutil.which("cmake")
        if self.cmake is None:
            self.skipTest("cmake not installed")

    def run_with(self, names):
        import subprocess, tempfile
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp)
            for name, real in names.items():
                path = bin_dir / name
                if real:
                    path.symlink_to(sys.executable)
                else:
                    path.write_text("#!/bin/sh\necho 'Python was not found' >&2\nexit 9009\n")
                    path.chmod(0o755)
            script = bin_dir / "probe.cmake"
            script.write_text(f'set(CMAKE_CURRENT_SOURCE_DIR "{REPO.as_posix()}")\n'
                              f'include("{(REPO / "cmake" / "find_python.cmake").as_posix()}")\n'
                              'message(STATUS "PICKED=${TEMPLATE_PYTHON} OK=${_tpl_python_ok}")\n')
            env = dict(os.environ, PATH=str(bin_dir))
            got = subprocess.run([self.cmake, "-P", str(script)], env=env,
                                 capture_output=True, text=True)
            picked = re.search(r"PICKED=(\S*) OK=(\S+)", got.stdout + got.stderr)
            self.assertIsNotNone(picked, got.stdout + got.stderr)
            return Path(picked.group(1)).name, picked.group(2)

    def test_a_stub_python3_is_passed_over_for_a_working_one(self):
        name, ok = self.run_with({"python3": False, "python3.12": True})
        self.assertEqual((name, ok), ("python3.12", "TRUE"))

    def test_py_is_found_when_nothing_else_works(self):
        name, ok = self.run_with({"python3": False, "python": False, "py": True})
        self.assertEqual((name, ok), ("py", "TRUE"))

    def test_nothing_that_works_is_nothing(self):
        name, ok = self.run_with({"python3": False, "python": False})
        self.assertEqual(ok, "FALSE")

    def test_the_hook_uses_it(self):
        hook = (REPO / "cmake" / "configure_hook.cmake").read_text()
        self.assertIn("cmake/find_python.cmake", hook)
        self.assertNotIn("find_program(TEMPLATE_PYTHON", hook)


class ConfigureProjectNameTest(unittest.TestCase):
    """A name that passes NAME_RE and still cannot build is refused, with a
    reason: a CMake target the build already defines, or a Windows device."""

    def reject(self, name):
        cfg = base_config(project={"name": name})
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        self.assertIsNotNone(cfgmod.locate_from(caught.exception))
        return str(caught.exception)

    def test_the_targets_the_build_already_defines(self):
        for name in sorted(cfgmod.RESERVED_NAMES):
            with self.subTest(name=name):
                self.assertIn("already a target of the build", self.reject(name))

    def test_windows_device_names_in_any_case(self):
        for name in ("con", "CON", "Nul", "aux", "prn", "com1", "COM9", "lpt3"):
            with self.subTest(name=name):
                self.assertIn("device name on Windows", self.reject(name))

    def test_names_that_only_look_like_them_are_fine(self):
        for name in ("console", "rmp_game", "raylib-fun", "com10", "unit", "my_game"):
            with self.subTest(name=name):
                with quiet():
                    cfgmod.validate(base_config(project={"name": name}), False)

    def test_the_reserved_set_is_every_target_the_cmake_files_define(self):
        """Read back out of the files, so a new target in CMakeLists.txt that a
        game could collide with fails here until it is in the set -- and in
        cmake/*.cmake, which CMakeLists.txt includes: game_test is defined in
        cmake/game_tests.cmake, where the first version of this did not look."""
        defined = set()
        for path in (REPO / "CMakeLists.txt", REPO / "thirdparty" / "raylib" / "src" / "CMakeLists.txt",
                     REPO / "thirdparty" / "raymob" / "CMakeLists.txt",
                     *sorted((REPO / "cmake").glob("*.cmake"))):
            for m in re.finditer(r"\b(?:add_executable|add_library|add_custom_target)\(\s*([A-Za-z_][A-Za-z0-9_]*)\b",
                                 path.read_text()):
                defined.add(m.group(1))
        self.assertGreater(len(defined), 5)
        self.assertEqual(defined - cfgmod.RESERVED_NAMES, set(),
                         "a target the build defines is not refused as a project name")


class ConfigureListValueTest(unittest.TestCase):
    """A list where a list goes, and a named error where it does not.

    ConfigureMembershipTest exists because `x in some_set` raises TypeError on
    an unhashable value. The same hole survived one level up, in the keys that
    are ITERATED rather than tested: `[targets] enabled = 5` came back as
    `TypeError: 'int' object is not iterable` from inside expand_targets, and
    `enabled = "all"` -- the most natural typo of the lot, because a group name
    is one word -- was accepted as the characters a, l and l and rejected with
    "unknown target or group 'a'", a message about a letter.
    """

    LIST_KEYS = [("targets", "enabled"), ("targets", "disabled"),
                 ("upx", "enabled"), ("upx", "disabled"),
                 ("raylib", "disabled_modules")]
    HOSTILE = [5, 3.5, True, None, {"a": 1}, ["ok", 5]]

    def with_value(self, section, key, value):
        table = copy.deepcopy(cfgmod.DEFAULTS[section])
        table[key] = value
        return base_config(**{section: table})

    def test_a_non_list_is_a_named_error_and_not_a_traceback(self):
        for section, key in self.LIST_KEYS:
            for value in self.HOSTILE:
                with self.subTest(option=f"[{section}] {key}", value=value):
                    with self.assertRaises(cfgmod.ConfigError), quiet():
                        cfgmod.validate(self.with_value(section, key, value), False)

    def test_a_bare_string_says_to_add_the_brackets(self):
        """`enabled = "all"` is iterable, which is exactly why it is dangerous:
        nothing crashes, it just means something else."""
        for section, key in self.LIST_KEYS:
            with self.subTest(option=f"[{section}] {key}"):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.with_value(section, key, "all"), False)
                self.assertIn("brackets", str(caught.exception))


class ConfigureStringValueTest(unittest.TestCase):
    """The keys that are read AS strings, and what a TOML array does to them.

    `[window] title = ["My Game"]` used to pass validate() outright, because
    `"\n" in ["My Game"]` is a perfectly good membership test on a list. It
    then reached gen_app_config and died with `'list' object has no attribute
    'replace'` -- a traceback, from a generator, about a config typo.
    """

    STRING_KEYS = [("project", "name"), ("window", "title"),
                   ("android", "application_id"), ("ios", "bundle_id"),
                   ("icon", "adaptive_background"), ("resources", "rres_password")]
    HOSTILE = [["a"], {"a": "b"}, 5, 3.5, True, None]

    def with_value(self, section, key, value):
        table = copy.deepcopy(cfgmod.DEFAULTS[section])
        table[key] = value
        return base_config(**{section: table})

    def test_a_non_string_never_reaches_a_generator(self):
        for section, key in self.STRING_KEYS:
            for value in self.HOSTILE:
                with self.subTest(option=f"[{section}] {key}", value=value):
                    with self.assertRaises(cfgmod.ConfigError), quiet():
                        cfgmod.validate(self.with_value(section, key, value), False)

    def test_a_list_title_does_not_reach_the_generated_header(self):
        """End to end, because that is where it landed: validate() accepted it
        and gen_app_config() raised AttributeError."""
        cfg = base_config()
        cfg["window"]["title"] = ["My Game"]
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(cfg, False)


class ConfigureRresPasswordTest(unittest.TestCase):
    """[resources] rres_password is one of exactly two config values that end
    up INSIDE the shipped binary, and it had no validation at all."""

    def test_an_empty_password_is_refused_and_says_there_is_no_unencrypted_pack(self):
        """It used to say: ship loose files instead, `rmp unpack` locally and
        leave resources/ in the archive. The release jobs build the pack
        themselves whatever the checkout holds, so that was a way out that led
        nowhere. What is true is that there is none."""
        cfg = base_config()
        cfg["resources"]["rres_password"] = ""
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(cfg, False)
        message = str(caught.exception)
        self.assertIn("AES key of nothing", message)
        self.assertIn("no setting for an unencrypted pack", message)
        self.assertNotIn("leave resources/ in the archive", message)

    def test_the_default_password_reaches_the_generated_header(self):
        with generated_header(base_config()) as text:
            self.assertIn("RMP_RRES_PASSWORD", text)


class ConfigureVersionCodeCeilingTest(unittest.TestCase):
    """versionCode is major*1_000_000 + minor*1_000 + patch, and Google Play
    refuses anything above 2,100,000,000. minor and patch were bounded; major
    was not, so v2101.0.0 produced a code the store rejects -- and an Android
    version code is a number you can never go back down from."""

    def code_for(self, tag):
        os.environ["GITHUB_REF_TYPE"] = "tag"
        os.environ["GITHUB_REF_NAME"] = tag
        try:
            return cfgmod.resolve_version()
        finally:
            os.environ.pop("GITHUB_REF_TYPE", None)
            os.environ.pop("GITHUB_REF_NAME", None)

    def test_a_major_past_the_play_ceiling_is_rejected(self):
        with self.assertRaises(cfgmod.ConfigError) as caught:
            self.code_for("v2100.0.0")
        self.assertIn("2,100,000,000", str(caught.exception))

    def test_the_hard_limit_itself_is_the_last_accepted_value(self):
        name, code = self.code_for("v2099.999.999")
        self.assertEqual(name, "2099.999.999")
        self.assertEqual(code, 2_099_999_999)
        self.assertLess(code, cfgmod.PLAY_MAX_VERSION_CODE)

    def test_the_constants_agree_with_each_other(self):
        """PLAY_MAX_MAJOR is derived from PLAY_MAX_VERSION_CODE, so they cannot
        be bumped independently."""
        self.assertLess(cfgmod.PLAY_MAX_MAJOR * 1_000_000 + 999_000 + 999,
                        cfgmod.PLAY_MAX_VERSION_CODE)
        self.assertGreaterEqual((cfgmod.PLAY_MAX_MAJOR + 1) * 1_000_000,
                                cfgmod.PLAY_MAX_VERSION_CODE)


# ---------------------------------------------------------------------------
# The workflows, read as data. Everything below is a fact about CI that went
# wrong once and is cheaper to assert than to remember.
# ---------------------------------------------------------------------------

WORKFLOWS = sorted((REPO / ".github" / "workflows").glob("*.yml"))


def job_block(path: Path, job: str) -> str:
    """The text of one job in a workflow, by indentation.

    Not a YAML parse: this has to work without PyYAML on a laptop, and the
    things being asserted below are about the TEXT -- a `default:` on an input,
    a step that runs a script -- which a parse would normalise away.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    out, inside = [], False
    for line in lines:
        if re.match(rf"^  {re.escape(job)}:\s*$", line):
            inside = True
            continue
        if inside and line.strip() and not line.startswith("    "):
            break
        if inside:
            out.append(line)
    return "\n".join(out)


class ReleaseAttachesEveryTargetTest(unittest.TestCase):
    """Every target that produces an artifact is attached to the release.

    linux-x64-musl and linux-arm64-glibc-drm were not, from the day they were
    added. Both built, both passed their gates, both uploaded an artifact, and
    neither was ever a release asset -- while SHA256SUMS listed them, because
    that step globs whatever was downloaded rather than what was published. So
    an Alpine user got nothing, and anyone running `sha256sum -c SHA256SUMS`
    got "No such file or directory" and a non-zero exit, which reads exactly
    like a tampered download.

    The list was hand-maintained. This is what makes it a derived fact.
    """

    # Targets with no zip of their own, and why. iOS ships as an xcframework
    # and a simulator app, both attached by name; android ships an apk and an
    # aab; web ships web-build.zip.
    NOT_A_TARGET_ZIP = {
        "ios": "ios-raylib-xcframework.zip and ios-app-simulator",
        "android": "an .apk and an .aab, attached by their own globs",
        "web": "web-build.zip",
        "freebsd-x64": "bsd-*-build/*.zip covers all five BSD legs",
        "freebsd-arm64": "bsd-*-build/*.zip",
        "openbsd-x64": "bsd-*-build/*.zip",
        "openbsd-arm64": "bsd-*-build/*.zip",
        "netbsd-x64": "bsd-*-build/*.zip",
    }

    def files_block(self):
        text = (REPO / ".github" / "workflows" / "_release.yml").read_text()
        block = text.split("files: |", 1)[1]
        out = []
        for line in block.splitlines()[1:]:
            if line.strip() and not line.startswith("            "):
                break
            if line.strip():
                out.append(line.strip())
        return out

    def test_every_target_zip_is_attached_to_the_release(self):
        attached = " ".join(self.files_block())
        self.assertIn("SHA256SUMS", attached, "the file list did not parse")
        for target in cfgmod.TARGETS:
            if target in self.NOT_A_TARGET_ZIP:
                continue
            with self.subTest(target=target):
                self.assertIn(f"{target}-build.zip", attached,
                              f"{target} uploads {target}-build but the release job "
                              f"never attaches it. SHA256SUMS will list a file that "
                              f"is not there.")

    def test_the_uploaded_artifact_names_are_the_ones_the_release_expects(self):
        """The other half: the release globs `<name>-build/<name>-build.zip`,
        so the artifact NAME has to match the target id."""
        uploads = "\n".join(p.read_text() for p in WORKFLOWS)
        for target in cfgmod.TARGETS:
            if target in self.NOT_A_TARGET_ZIP:
                continue
            with self.subTest(target=target):
                self.assertIn(f"name: {target}-build", uploads)

    def test_every_target_has_an_itch_channel_or_a_written_reason(self):
        """itch.io publishes a subset, on purpose. The subset has to be a
        DECISION and not an oversight -- which is exactly what musl and the two
        DRM targets were."""
        text = (REPO / ".github" / "workflows" / "_itch.yml").read_text()
        for target in cfgmod.TARGETS:
            with self.subTest(target=target):
                has_channel = bool(
                    re.search(rf"artifact: {re.escape(target)}-build\b", text)
                    # (?<![-\w]) or the `no-itch-channel: ios` line below counts
                    # as `channel: ios` and every excluded target reads as
                    # published as well.
                    or re.search(rf"(?<![-\w])channel: {re.escape(target)}(?![\w-])", text))
                excluded = bool(
                    re.search(rf"no-itch-channel: {re.escape(target)}(?![\w-])", text))
                self.assertTrue(
                    has_channel or excluded,
                    f"{target} has neither an itch channel nor a "
                    f"`# no-itch-channel: {target} — <why>` line in _itch.yml.")
                self.assertFalse(has_channel and excluded,
                                 f"{target} is both published and excluded")


class NoJobLevelContinueOnErrorTest(unittest.TestCase):
    """`continue-on-error` on a JOB reports a failed job as a success.

    _itch.yml had it next to `fail-fast: false`, which already gives the thing
    the comment claimed it was for -- matrix legs are independent without it.
    All it added was that a butler push that 401'd published nothing and the
    run was entirely green. _firebase.yml had it twice, where the
    steps.cfg.outputs.ok guard already handles "not configured".

    Step level is a different thing and is left alone: a step that is allowed
    to fail is usually a diagnostic, and _apple.yml's note on the iOS simulator
    test is the argument for why even that is usually wrong.
    """

    def test_no_job_declares_continue_on_error(self):
        offenders = []
        for path in WORKFLOWS:
            for n, line in enumerate(path.read_text().splitlines(), 1):
                # Four spaces = a key of a job. Eight = a key of a step.
                if re.match(r"^    continue-on-error:\s*true\s*$", line):
                    offenders.append(f"{path.name}:{n}")
        self.assertEqual(offenders, [],
                         "a job-level continue-on-error reports failure as success:\n  "
                         + "\n  ".join(offenders))


class PinnedInputsHaveNoDefaultTest(unittest.TestCase):
    """A pin lives once, in thirdparty/FROZEN_VERSIONS.md.

    _apple.yml's own header said "ci.yml always passes explicit values read
    from there, so this file is never the source of truth" while ci.yml passed
    none of them -- so every Apple build ran on the defaults in the callee, and
    versions_check.sh never compared them because it did not know they existed.
    The pins now travel `configure.py --print-pins` -> the config job's outputs
    -> these inputs, and the inputs have no defaults to fall back to.
    """

    PINNED = {
        "_apple.yml": ["runner", "xcode_version", "ninja_version", "ninja_sha256",
                       "xcodegen_version", "xcodegen_sha256"],
        "_windows.yml": ["runner", "mesa_version", "mesa_sha256"],
    }

    def inputs_of(self, name):
        """{input name: its block of text}, from the workflow_call inputs."""
        text = (REPO / ".github" / "workflows" / name).read_text()
        body = text.split("inputs:", 1)[1]
        out, current = {}, None
        for line in body.splitlines():
            if re.match(r"^\S", line):
                break
            m = re.match(r"^      ([a-z0-9_]+):\s*$", line)
            if m:
                current = m.group(1)
                out[current] = []
                continue
            if current and line.startswith("        "):
                out[current].append(line.strip())
        return {k: "\n".join(v) for k, v in out.items()}

    def test_no_pinned_input_carries_a_default(self):
        for name, pins in self.PINNED.items():
            declared = self.inputs_of(name)
            for pin in pins:
                with self.subTest(workflow=name, input=pin):
                    self.assertIn(pin, declared, f"{name} no longer declares {pin}")
                    self.assertNotIn("default:", declared[pin],
                                     f"{name}: input `{pin}` has a default, which is a "
                                     f"second home for a number that lives in "
                                     f"thirdparty/FROZEN_VERSIONS.md")
                    self.assertIn("required: true", declared[pin])

    def test_every_caller_passes_every_pin(self):
        callers = {"ci.yml": ("windows", "apple"), "canary.yml": ("windows", "apple")}
        for caller, jobs in callers.items():
            text = (REPO / ".github" / "workflows" / caller).read_text()
            for job in jobs:
                block = job_block(REPO / ".github" / "workflows" / caller, job)
                self.assertTrue(block, f"{caller} has no job called {job}")
                name = "_apple.yml" if job == "apple" else "_windows.yml"
                for pin in self.PINNED[name]:
                    with self.subTest(caller=caller, job=job, input=pin):
                        self.assertRegex(block, rf"(?m)^\s+{pin}:",
                                         f"{caller}: job `{job}` does not pass `{pin}` to "
                                         f"{name}, which now requires it")

    def test_the_pins_the_config_job_publishes_exist_in_the_frozen_block(self):
        pins = cfgmod.frozen_versions()
        for key in ("macos_runner", "xcode", "ninja_mac", "ninja_mac_sha256",
                    "xcodegen", "xcodegen_sha256", "windows_runner", "mesa", "mesa_sha256"):
            self.assertIn(key, pins)


class LintJobTest(unittest.TestCase):
    """What the CI lint job has to run, named one by one where a regression
    would be invisible. That every stage of `rmp test` has its step there is
    StagesAgreeWithLintTest, in tests/rmp_test.py, which reads the stages from
    tools/rmp.py itself.

    ui_layout_test is why that comparison exists. `-DBUILD_UI_TESTS=ON`
    appeared in ci.yml exactly once, inside the clang-tidy step, so that the
    file got a real compile_commands.json entry -- and nothing ever built or
    ran the binary. The headless UI layout assertions, four resolutions and no
    GPU, ran on developer machines only for four phases.
    """

    def test_the_lint_job_runs_the_sanitized_unit_tests(self):
        """`rmp test sanitize` is not in `rmp test` -- it is a second build --
        so CI is where it has to run every time."""
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("bash tools/sanitize_check.sh", lint)
        self.assertIn('Stage("sanitize"', (REPO / "tools" / "rmp.py").read_text())

    def test_the_lint_job_runs_the_headless_ui_layout_test(self):
        """Named on its own because it is the one that was missing, and a
        regression here is invisible: the binary still BUILDS in the clang-tidy
        step's configure."""
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("--target ui_layout_test", lint)
        self.assertIn("./build/debug/ui_layout_test", lint)

    def test_actionlint_asserts_shellcheck_is_there(self):
        """actionlint shellchecks every `run:` block IF shellcheck is on PATH,
        and says nothing when it is not -- so the day the image loses it, this
        step keeps printing PASS having checked half of what it used to."""
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("command -v shellcheck", lint)



def step_block(job: str, name: str) -> str:
    """The text of one step of a job_block(), by its `- name:`."""
    at = job.index(f"      - name: {name}\n")
    end = job.find("\n      - ", at + 1)
    return job[at:] if end < 0 else job[at:end]


class CiRequiresSanitizersTest(unittest.TestCase):
    """Where CI builds the framework in Debug, a configure that could not
    instrument it has to fail -- cmake/sanitize.cmake warns and goes on
    everywhere else, which on a runner would be a gate that passes by not
    looking. RMP_REQUIRE_SANITIZERS=1 is what makes it fail, set on exactly
    the steps whose builds are gates."""

    CI = REPO / ".github" / "workflows" / "ci.yml"

    def test_the_lint_jobs_debug_steps_require_them(self):
        lint = job_block(self.CI, "lint")
        for name in ("Unit tests", "UI layout tests (headless)", "Render (software, no GPU)"):
            with self.subTest(step=name):
                self.assertIn('RMP_REQUIRE_SANITIZERS: "1"', step_block(lint, name))

    def test_the_examples_job_requires_them_and_boots_on_x11(self):
        examples = job_block(self.CI, "examples")
        head = examples[:examples.index("    steps:")]
        self.assertIn('RMP_REQUIRE_SANITIZERS: "1"', head)
        smoke = step_block(examples, "Smoke on X11 and Mesa (xvfb)")
        self.assertIn("xvfb-run -a ./rmp test smoke", smoke)

    def test_the_gcc_run_requires_them_itself(self):
        script = (REPO / "tools" / "sanitize_check.sh").read_text()
        self.assertIn("RMP_REQUIRE_SANITIZERS=1 cmake", script)
        self.assertIn("-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++", script)
        self.assertNotIn("clang++", script.split("set -euo pipefail", 1)[1],
                         "the always-on Debug build is the clang run; this one is gcc's")
        self.assertIn("=== SANITIZERS: address, undefined ===", script)

class PackagingShipsOneArchiveShapeTest(unittest.TestCase):
    """Every release archive has the same contents, or says why not.

    Four jobs shipped `resources.rres`; five shipped a loose `resources/`
    folder, three of them for no stated reason. Both work -- the loader falls
    back to loose files -- so the difference was invisible until somebody
    compared two downloads.

    The rule: pack where the packer can run (native builds), ship loose with a
    PACK_SKIPPED.txt naming the reason where it cannot (cross-compiled
    targets). The test is what stops a sixth shape appearing.
    """

    # Cross-compiled: rres_pack comes out for the TARGET and cannot run on the
    # host that built it. Each of these must ship loose files AND the note.
    CROSS = ("linux-x64-musl", "linux-riscv64-glibc", "windows-arm64")

    def package_steps(self):
        """(workflow, step text) for every step that assembles `package/`.

        Keyed on `package/resources`, which is what a desktop archive is: a
        binary, a resources folder and the licence notice. Web and the iOS
        .app are deliberately not in it -- the browser bundle embeds its assets
        in the wasm package and the .app carries them inside the bundle, so
        neither has an archive shape to be consistent with.
        """
        out = []
        for path in WORKFLOWS:
            lines = path.read_text().splitlines()
            for i, line in enumerate(lines):
                if not re.match(r"^      - name: Package\b", line):
                    continue
                body = []
                for follow in lines[i + 1:]:
                    if follow.strip() and not follow.startswith("        "):
                        break
                    body.append(follow)
                text = "\n".join(body)
                if "package/resources" in text:
                    out.append((path.name, text))
        return out

    def test_there_are_as_many_packaging_steps_as_there_are_archives(self):
        steps = self.package_steps()
        # six Linux, one macOS, two Windows, one BSD.
        self.assertGreaterEqual(len(steps), 10,
                                "the Package steps stopped parsing, and a scan that "
                                "finds nothing passes every assertion below")
        self.assertEqual({"_linux.yml", "_apple.yml", "_windows.yml", "_bsd.yml"},
                         {name for name, _ in steps})

    def test_every_native_archive_ships_through_the_one_script(self):
        """`cp resources/resources.rres package/resources/` was the whole of it,
        so a game in plain C -- which raw raylib cannot read a pack for --
        shipped a release that found none of its files. tools/ship_resources.sh
        decides, and _windows.yml says the same in PowerShell."""
        for name, body in self.package_steps():
            if "PACK_SKIPPED.txt" in body and "ship_resources" not in body:
                continue  # a cross-compiled target: loose, with the note
            with self.subTest(workflow=name, step=body.splitlines()[0][:60]):
                self.assertNotIn("cp resources/resources.rres package/resources/", body)
                self.assertIn("ship_resources", body)

    def ship(self, framework: bool, packed: bool):
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "resources" / "art").mkdir(parents=True)
            (root / "resources" / "rabbit.png").write_bytes(b"png")
            (root / "resources" / "art" / "tree.png").write_bytes(b"png")
            if packed:
                (root / "resources" / "resources.rres").write_bytes(b"rres")
            if framework:
                (root / "src" / "rmp").mkdir(parents=True)
            got = subprocess.run(["sh", str(REPO / "tools" / "ship_resources.sh"), "package"],
                                 cwd=root, capture_output=True, text=True)
            shipped = sorted(p.relative_to(root / "package").as_posix()
                             for p in (root / "package").rglob("*") if p.is_file())
            return got.returncode, shipped

    def test_a_framework_game_ships_the_pack_alone(self):
        self.assertEqual(self.ship(framework=True, packed=True),
                         (0, ["resources/resources.rres"]))
        code, _ = self.ship(framework=True, packed=False)
        self.assertNotEqual(code, 0, "a framework game with no pack is a failed release")

    def test_a_plain_c_game_ships_its_loose_files_and_says_why(self):
        code, shipped = self.ship(framework=False, packed=True)
        self.assertEqual(code, 0)
        self.assertEqual(shipped, ["resources/PACK_SKIPPED.txt", "resources/art/tree.png",
                                   "resources/rabbit.png"])

    def test_every_archive_ships_the_pack_or_says_why_it_does_not(self):
        for name, body in self.package_steps():
            with self.subTest(workflow=name, step=body.splitlines()[0][:60]):
                packs = ("resources.rres package/resources/" in body
                         or "ship_resources" in body)
                loose = ("cp -r resources package/" in body
                         or "Copy-Item resources package/ -Recurse" in body)
                self.assertTrue(packs or loose, "this step ships no resources at all")
                self.assertFalse(packs and loose, "this step ships both shapes")
                if loose:
                    self.assertIn(
                        "PACK_SKIPPED.txt", body,
                        "a loose-resources archive has to say why in the zip: every "
                        "other archive carries resources.rres, and an undocumented "
                        "difference is the bug this test exists for.")

    def test_the_cross_compiled_targets_are_the_only_loose_ones(self):
        loose = []
        for name, body in self.package_steps():
            # ship_resources' own PACK_SKIPPED is the plain C game's, which any
            # native archive can be; this counts the ones that are always loose.
            if "PACK_SKIPPED.txt" in body and "ship_resources" not in body:
                loose.append(body)
        self.assertEqual(len(loose), len(self.CROSS),
                         f"expected exactly {len(self.CROSS)} loose-resource archives "
                         f"({', '.join(self.CROSS)}), found {len(loose)}")
        joined = "\n".join(loose)
        for target in self.CROSS:
            self.assertIn(target, joined)

    def test_every_archive_carries_the_licence_notice(self):
        for name, body in self.package_steps():
            with self.subTest(workflow=name):
                self.assertIn("LICENSES.txt", body)


class GameLaunchesReadNothingFromStdinTest(unittest.TestCase):
    """A game a script starts reads nothing from the script's stdin.

    cross-platform-actions feeds the BSD step's script to the VM's shell on
    STDIN. raylib's headless platform (rcore_memory.c) polls the keyboard with
    a non-blocking getchar(), and stdio reads a whole buffer from the fd: the
    game ate the rest of the script. The shell then met a cut line -- "sh: 59:
    Syntax error: Unterminated quoted string" at the last line of a 3.1 KB
    script, after shipped_check.sh -- or simply ran out of script and exited 0.
    render_check.sh survived only by being the last line. Every launch of a
    game in tools/ takes its stdin from /dev/null.
    """

    def launches(self):
        for f in sorted((REPO / "tools").glob("*.sh")):
            for n, line in enumerate(f.read_text().splitlines(), 1):
                code = line.split("#", 1)[0] if not line.lstrip().startswith("#") else ""
                if re.search(r"\bRAY_TEST_MAX_FRAMES=", code):
                    yield f.name, n, code

    def test_every_launch_reads_dev_null(self):
        found = list(self.launches())
        self.assertGreaterEqual(len(found), 4)
        for name, n, code in found:
            with self.subTest(at=f"{name}:{n}"):
                self.assertRegex(code, r"<\s*/dev/null", f"{name}:{n} starts a game that can read the script's stdin")



FAKE_CMAKE = r"""#!/bin/sh
# Configures by writing the game the run is about, and builds by doing nothing.
case " $* " in *" --build "*) exit 0 ;; esac
mkdir -p build/memory
cat > build/memory/game <<'GAME'
#!/bin/sh
echo "INFO: RAY_TEST_BOOT_OK assets_failed=0 assets_requested=1 vsync=1"
echo "INFO: RAY_TEST_RENDER_OK frame=5 pixels=1 ratio=0.1 hash=0badf00d"
echo "INFO: RAY_TEST_DONE_FRAMES rendered=10"
printf '%b' "$STUB_EXTRA"
exit "$STUB_STATUS"
GAME
chmod +x build/memory/game
"""


class RenderCheckReadsTheExitTest(unittest.TestCase):
    """tools/render_check.sh ended its launch in `|| true` and judged the run
    by the markers alone -- which are printed BEFORE the process ends. A Debug
    build is instrumented by [dev] sanitize, and LeakSanitizer reports at exit,
    after RAY_TEST_DONE_FRAMES, with a status nothing read. The script runs
    here in a copy of the tree, against a cmake that writes a stand-in game."""

    def run_check(self, status, extra=""):
        import shutil
        import subprocess
        if sys.platform == "win32" or shutil.which("sh") is None:
            self.skipTest("render_check.sh is POSIX sh")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tools").mkdir()
            shutil.copy(REPO / "tools" / "render_check.sh", root / "tools")
            (root / "include" / "rmp" / "generated").mkdir(parents=True)
            (root / "include" / "rmp" / "generated" / "config.h").write_text(
                "#define RMP_WINDOW_VSYNC 1\n")
            fake = root / "fakebin"
            fake.mkdir()
            (fake / "cmake").write_text(FAKE_CMAKE)
            (fake / "cmake").chmod(0o755)
            env = dict(os.environ, PATH=f"{fake}{os.pathsep}{os.environ['PATH']}",
                       STUB_STATUS=str(status), STUB_EXTRA=extra)
            got = subprocess.run(["sh", "tools/render_check.sh", "Ninja", "", "game"], cwd=root,
                                 env=env, capture_output=True, text=True,
                                 stdin=subprocess.DEVNULL)
            return got.returncode, got.stdout + got.stderr

    def test_a_clean_run_passes(self):
        code, out = self.run_check(0)
        self.assertEqual(code, 0, out)
        self.assertIn("PASS: booted, rendered and exited", out)

    def test_an_exit_status_after_every_marker_fails(self):
        code, out = self.run_check(23)
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: the game exited with status 23", out)

    def test_a_sanitizer_report_fails_whatever_the_status(self):
        for report in (r"==9==ERROR: LeakSanitizer: detected memory leaks\n",
                       r"==9==ERROR: AddressSanitizer: stack-use-after-return\n",
                       r"src/rmp/ui/x.cpp:1:2: runtime error: shift exponent 40\n"):
            for status in (0, 1):
                with self.subTest(report=report, status=status):
                    code, out = self.run_check(status, report)
                    self.assertEqual(code, 1, out)
                    self.assertIn("FAIL: a sanitizer reported the run above", out)
                    self.assertIn("the sanitizer's report", out)

class BsdInlineScriptSizeTest(unittest.TestCase):
    """The cpa.sh script has a size limit and nothing was measuring it.

    cross-platform-actions carries the step's `run:` block into the VM through
    its own shell and cuts it off somewhere between 4.8 KB (worked) and 5.6 KB
    (did not). Both cuts were silent: one landed inside a quoted string and
    came back as "Unterminated quoted string" on an unplaceable line, the other
    landed somewhere that parsed, so the shell hit EOF and EXITED 0 with half
    the step unrun -- a green that proved nothing.

    portable_check.sh enforces it; this checks the same number independently,
    because the two would have to break the same way to both be wrong.
    """

    LIMIT = 4096

    def block(self):
        lines = (REPO / ".github" / "workflows" / "_bsd.yml").read_text().splitlines()
        start = next(i for i, l in enumerate(lines)
                     if l.strip() == "run: |" and "Install deps" in lines[i - 1])
        indent = len(lines[start]) - len(lines[start].lstrip())
        out = []
        for line in lines[start + 1:]:
            if line.strip() and (len(line) - len(line.lstrip())) <= indent:
                break
            out.append(line[indent + 2:] if len(line) > indent else line)
        return "\n".join(out)

    def test_the_script_carried_into_the_vm_is_under_the_limit(self):
        size = len(self.block().encode("utf-8"))
        self.assertLess(size, self.LIMIT,
                        f"the cpa.sh script is {size} bytes. Move the work into a "
                        f"committed file the way tools/render_check.sh did.")

    def test_it_is_pure_ascii(self):
        """An em dash inside a double-quoted echo came back as
        `sh: 83: Syntax error: Unterminated quoted string`, from a line number
        belonging to a generated script, in a step whose real job was to build
        the game."""
        body = self.block()
        bad = [c for c in body if ord(c) > 126 or (ord(c) < 32 and c not in "\n\t")]
        self.assertEqual(bad, [], f"non-ASCII in the cpa.sh block: {bad!r}")

    def test_portable_check_enforces_the_same_limit(self):
        text = (REPO / "tools" / "portable_check.sh").read_text()
        self.assertIn(f"BSD_LIMIT={self.LIMIT}", text,
                      "the script and this test have to agree on the number")

    def test_the_last_line_of_the_vm_script_is_the_render_check(self):
        """Anything after it can simply not be there: the truncation lands
        where it lands, and the completion marker is written from inside
        render_check.sh so there is nothing after the call to lose."""
        last = [l for l in self.block().splitlines() if l.strip()][-1]
        self.assertIn("tools/render_check.sh", last)


class ShellPatternCheckTest(unittest.TestCase):
    """`A && B || C` is not if/then/else, and shellcheck does not say so.

    SC2015 only fires on a couple of degenerate shapes. Every occurrence this
    repository has shipped was of a shape it says nothing about: it was
    written, fixed, and then written again in seven more places two rounds
    later. CLAUDE.md credited actionlint with catching it; actionlint does not.
    """

    SCRIPT = REPO / "tools" / "shell_pattern_check.sh"

    def run_on(self, *lines, suffix=".sh"):
        import subprocess, tempfile
        with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as fh:
            fh.write("\n".join(lines) + "\n")
            name = fh.name
        try:
            return subprocess.run(["bash", str(self.SCRIPT), name],
                                  capture_output=True, text=True)
        finally:
            os.unlink(name)

    def test_it_goes_red_on_the_shape_it_exists_for(self):
        got = self.run_on('make_it && echo ok || exit 1')
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("not if/then/else", got.stdout)

    def test_the_guard_shape_is_allowed(self):
        """`cmd || { echo ...; exit 1; }` has no `&&`, which is the whole
        reason it is the shape to reach for."""
        got = self.run_on('make_it || { echo "FAIL: no"; exit 1; }',
                          'if make_it; then echo ok; else echo no; exit 1; fi')
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_a_string_or_a_comment_is_not_code(self):
        """portable_check.sh's own advice string contains `&& pwd` inside double
        quotes on a line that really does end in `|| fails=...`. A rule that
        flags that is a rule somebody switches off within a week."""
        got = self.run_on('echo "cd \\"$(dirname \\"$0\\")\\" && pwd" || fails=1',
                          '# A && B || C is what this forbids')
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_a_github_expression_is_not_shell(self):
        """`${{ a && b || c }}` really is a ternary, in GitHub's language, and
        it never reaches a shell."""
        got = self.run_on("NAME: ${{ inputs.x == 'y' && 'a' || 'b' }}")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_the_whole_tree_is_clean(self):
        import subprocess
        got = subprocess.run(["bash", str(self.SCRIPT)], cwd=REPO,
                             capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_it_is_wired_into_rmp_test_and_the_lint_job(self):
        self.assertIn('"tools/shell_pattern_check.sh"', (REPO / "tools" / "rmp.py").read_text())
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("shell_pattern_check.sh", lint)


class NamingCheckTest(unittest.TestCase):
    """tools/naming_check.sh: the naming rules clang-tidy cannot see.

    clang-tidy reads one branch of every #if, so the web, iOS, Android and
    Windows code is invisible to a Linux lint run; it cannot see member access
    (`foo_.x` is a use, not a declaration); and a retired macro in `#if` is not
    an error but 0. Every rule is proven red here on a fixture of its own.
    """

    SCRIPT = REPO / "tools" / "naming_check.sh"

    def run_on(self, *lines, suffix=".cpp"):
        import subprocess, tempfile
        with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as fh:
            fh.write("\n".join(lines) + "\n")
            name = fh.name
        try:
            return subprocess.run(["bash", str(self.SCRIPT), name],
                                  capture_output=True, text=True)
        finally:
            os.unlink(name)

    def assert_red(self, rule, *lines, suffix=".cpp"):
        got = self.run_on(*lines, suffix=suffix)
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn(f" {rule} ", got.stdout)

    def assert_green(self, *lines, suffix=".cpp"):
        got = self.run_on(*lines, suffix=suffix)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_r1_a_g_name(self):
        self.assert_red("R1", "namespace { bool g_started = false; }")

    def test_r2_a_k_constant(self):
        self.assert_red("R2", "constexpr int kMaxLabels = 64;")

    def test_r3_an_underscore_before_a_dot(self):
        self.assert_red("R3", "float f(const Box &box_) { return box_.x; }")

    def test_r3_an_underscore_before_an_arrow(self):
        self.assert_red("R3", "int f(Slot *slot_) { return slot_->count; }")

    def test_r4_another_objects_private_member(self):
        self.assert_red("R4", "Resource(const Resource &other) : _slot(other._slot) {}")
        self.assert_red("R4", "void f(Object *o) { o->_index = 0; }")

    def test_r4_allows_a_call_to_a_hook(self):
        self.assert_green("void f(Object &a, Object &b) { a._collision(b); b->_update(1); }",
                          "void g(B *b, Object &o) { b->_late_update(o, 0); }")

    def test_r5_a_macro_without_the_prefix(self):
        self.assert_red("R5", "#define SCREEN_SIZE 800")

    def test_r5_a_vendored_knob_outside_its_own_file(self):
        """CLAY_IMPLEMENTATION is allowed in clay_impl.cpp and nowhere else."""
        self.assert_red("R5", "#define CLAY_IMPLEMENTATION")

    def test_r5_allows_our_prefix(self):
        self.assert_green("#define RMP_THING 1")

    def test_r8_a_trailing_underscore(self):
        """"No trailing `_`" is in the naming table, and nothing checked it:
        RMP_REPORT_ONCE declared `rmp_report_site_` for months. clang-tidy
        does not name a declaration a macro expands, and R3 only sees the
        underscore when a dot or an arrow follows it."""
        self.assert_red("R8", "#define RMP_ONCE() do { static const char site_ = 0; } while (0)")
        internal = (REPO / "src" / "rmp" / "internal.h").read_text()
        self.assertNotIn("rmp_report_site_", internal)
        self.assert_red("R8", "int count_ = 0;")
        self.assert_red("R8", "void f(int value_);")

    def test_r8_leaves_comments_strings_and_the_foreign_field_alone(self):
        self.assert_green("// the move_* actions", 'const char *name = "many_";',
                          "int f(Layer *layer) { return layer->class_.ptr != nullptr; }",
                          "#define RMP_ARGS(...) f(__VA_ARGS__)")

    def test_r6_a_constant_named_like_a_raylib_macro(self):
        self.assert_red("R6", "constexpr float PI = 3.14159f;")
        self.assert_red("R6", "constexpr float EPSILON = 0.000001f;")

    def test_r6_a_constant_named_like_a_bsd_macro(self):
        """MIN and MAX come from <sys/param.h> on the BSDs and macOS -- and from
        rlgl.h -- so the preprocessor rewrites them on some platforms only."""
        self.assert_red("R6", "constexpr int MIN = 0;")
        self.assert_red("R6", "static const int MAX = 9;")

    def test_r6_an_enum_member_named_like_a_macro(self):
        self.assert_red("R6", "enum class Kind { RED, NOTHING };")

    def test_r6_a_constant_in_the_macros_namespace(self):
        self.assert_red("R6", "constexpr int RMP_LIMIT = 3;")

    def test_r6_allows_a_descriptive_name(self):
        self.assert_green("constexpr float NEAR_ZERO = 1e-6f;",
                          "constexpr int SECTION_COUNT = 3;",
                          "enum class Kind { SQUARE, CIRCLE };")

    def test_r7_a_retired_name(self):
        self.assert_red("R7", "#if APP_SAVE_PORTABLE && defined(_WIN32)", "#endif")
        self.assert_red("R7", "const char *root = RESOURCES_PATH;")

    def test_comments_and_strings_are_not_code(self):
        self.assert_green("// g_x, kX and foo_.x are what this forbids",
                          "/* kMax and other._x, across",
                          "   two lines */",
                          'const char *s = "g_x kX foo_.x a._b";',
                          "char c = 'k';",
                          'const char *raw = R"x(g_y "kZ" foo_.x)x";')

    def test_a_digit_separator_is_not_a_character_literal(self):
        """`1'000` must not open a character literal and blank the rest of the
        line -- which would hide the g_ name after it."""
        self.assert_red("R1", "int n = 1'000; int g_bad = 0;")

    def test_a_foreign_trailing_underscore_is_allowed(self):
        """cute_tiled's own field is `class_`; tilemap.cpp reads layer->class_.ptr."""
        self.assert_green("const char *c = layer->class_.ptr;")

    def test_a_local_runtime_constant_stays_snake_case(self):
        self.assert_green("void f(float w) { const float half = w / 2; (void)half; }")

    def test_the_vendored_macro_names_are_read(self):
        """R6's set is read from the vendored headers, live. If that parse
        broke, PI would quietly be allowed again; this says the set is there."""
        import subprocess
        got = subprocess.run(["bash", str(self.SCRIPT), "--macros"], cwd=REPO,
                             capture_output=True, text=True)
        names = set(got.stdout.split())
        self.assertGreater(len(names), 500, got.stderr)
        for name in ("PI", "EPSILON", "DEG2RAD", "MIN", "MAX", "RAYWHITE", "CHECK",
                     "CLAY_STRING", "DEBUG"):
            self.assertIn(name, names)
        self.assert_red("R6", "constexpr float DEG2RAD = 0.01745f;")

    def test_the_whole_tree_is_clean(self):
        import subprocess
        got = subprocess.run(["bash", str(self.SCRIPT)], cwd=REPO,
                             capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_every_rule_is_enforced_on_the_tree(self):
        """The rules joined ENFORCED one by one as each rename landed. All of
        them hold now, and a rule taken back out would let its mistake back in
        without a word."""
        text = self.SCRIPT.read_text()
        enforced = set(re.findall(r'"(R\d)"', re.search(r"^ENFORCED = \{([^}]*)\}",
                                                       text, re.M).group(1)))
        rules = set(re.findall(r'^    "(R\d)": ', text, re.M))
        self.assertEqual(len(rules), 8)
        self.assertEqual(enforced, rules)

    def test_it_is_wired_into_rmp_test_and_the_lint_job(self):
        self.assertIn('"tools/naming_check.sh"', (REPO / "tools" / "rmp.py").read_text())
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("naming_check.sh", lint)


class ClangTidyNamingTest(unittest.TestCase):
    """The naming rules in .clang-tidy, each seen red on a file of its own.

    Run where clang-tidy exists; the lint job sets RMP_REQUIRE_CLANG_TIDY=1, so
    there a missing clang-tidy is a failure and not a skip. The files are
    written INSIDE the tree (under a dot-directory that nothing globs), because
    clang-tidy resolves InheritParentConfig from the file's own directory --
    with --config-file it silently drops everything examples/.clang-tidy
    inherits, and a test of that would pass with no rules at all."""

    def setUp(self):
        import shutil
        self.tidy = shutil.which("clang-tidy")
        if self.tidy is None:
            if os.environ.get("RMP_REQUIRE_CLANG_TIDY") == "1":
                self.fail("clang-tidy is required here (RMP_REQUIRE_CLANG_TIDY=1)")
            self.skipTest("clang-tidy not installed")

    def tidy_on(self, where, *lines):
        import subprocess, tempfile, shutil
        tmp = Path(tempfile.mkdtemp(prefix=".tidy-probe-", dir=REPO / where))
        try:
            src = tmp / "probe.cpp"
            src.write_text("\n".join(lines) + "\n")
            got = subprocess.run([self.tidy, "--quiet", str(src), "--", "-std=c++20"],
                                 capture_output=True, text=True)
            return got.stdout + got.stderr
        finally:
            shutil.rmtree(tmp)

    BAD = (
        ("constexpr int kBad = 1;", "constexpr variable 'kBad'"),
        ("class C { int bad_ = 0; public: int get() const { return bad_; } };",
         "private member 'bad_'"),
        ("class C { public: void _helper() {} };", "method '_helper'"),
        ("#define BAD_THING 1", "macro definition 'BAD_THING'"),
        ("namespace { struct { int count = 0; } state; }\n"
         "int f(int state) { return state; }", "shadows"),
    )
    GOOD = ("constexpr int GOOD = 1;",
            "class C { int _good = 0; public: int get() const { return _good; } "
            "void _ready() {} };",
            "#define RMP_GOOD 1",
            "void f(float w) { const float half = w / 2; (void)half; }")

    def check(self, where):
        for line, said in self.BAD:
            with self.subTest(where=where, line=line):
                self.assertIn(said, self.tidy_on(where, line))
        out = self.tidy_on(where, *self.GOOD)
        self.assertNotIn("warning:", out, out)

    def test_the_framework_rules(self):
        self.check("src")

    def test_examples_inherit_them(self):
        self.check("examples")


class LintWiringTest(unittest.TestCase):
    """`rmp lint` and the CI lint job run the same script over the same three
    folders, against a compile database of their own."""

    def test_both_call_the_script(self):
        rmp = (REPO / "tools" / "rmp.py").read_text()
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn('"tools/lint.sh"', rmp)
        self.assertIn("bash tools/lint.sh", lint)
        # Nobody calls clang-tidy around it any more: one list of files.
        self.assertNotIn("clang-tidy -p", lint)
        self.assertNotIn('"clang-tidy"', rmp)

    def test_the_script_lints_src_tests_and_examples(self):
        text = (REPO / "tools" / "lint.sh").read_text()
        self.assertIn("find src tests examples -name '*.cpp'", text)
        self.assertIn("-p build/lint", text)
        self.assertIn("cmake --preset lint", text)

    def test_the_lint_preset_configures_tests_and_examples_elsewhere(self):
        presets = json.loads((REPO / "CMakePresets.json").read_text())
        lint = next(p for p in presets["configurePresets"] if p["name"] == "lint")
        self.assertEqual(lint["binaryDir"], "${sourceDir}/build/lint")
        for flag in ("BUILD_TESTS", "BUILD_UI_TESTS", "RMP_BUILD_EXAMPLES"):
            self.assertEqual(lint["cacheVariables"][flag], "ON")
        debug = next(p for p in presets["configurePresets"] if p["name"] == "debug")
        self.assertNotIn("RMP_BUILD_EXAMPLES", debug["cacheVariables"])

    def test_a_file_without_a_compile_command_fails(self):
        """The guess clang-tidy makes for a file it has no entry for is what
        once went green here and red on the runner. The script refuses it."""
        text = (REPO / "tools" / "lint.sh").read_text()
        self.assertIn("no compile command", text)
        self.assertIn("sys.exit(1)", text)

    def test_the_examples_config_keeps_the_names(self):
        text = (REPO / "examples" / ".clang-tidy").read_text()
        self.assertIn("InheritParentConfig: true", text)
        self.assertIn("readability-identifier-naming", text)

    def test_the_lint_job_requires_clang_tidy_for_the_tests(self):
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("RMP_REQUIRE_CLANG_TIDY", lint)


class LintReadsTheCTest(unittest.TestCase):
    """tools/lint.sh looked for *.cpp only, so the static analyzer had never
    read a line of C here -- and tools/rres_pack.c, which every desktop release
    runs, dereferenced the first allocation that failed. Every C file of ours
    is linted now; tools/.clang-tidy keeps the analyzer and drops two checks
    for the two tools, each with its reason."""

    def listed(self) -> set[str]:
        """What lint.sh's own `find` lines select, run here as written."""
        import subprocess
        text = (REPO / "tools" / "lint.sh").read_text()
        finds = [" ".join(f.split()) for f in
                 re.findall(r"^\s+(find [^\n]*(?:\n[^\n]*)??\| sort)\)$", text, re.M)]
        self.assertGreaterEqual(len(finds), 2, "lint.sh no longer has its two file lists")
        got = subprocess.run(["bash", "-c", "\n".join(finds)], cwd=REPO, capture_output=True,
                             text=True)
        self.assertEqual(got.returncode, 0, got.stderr)
        return set(got.stdout.split())

    def test_every_c_file_of_ours_is_linted(self):
        import subprocess
        tracked = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "*.c"], cwd=REPO,
                                 capture_output=True, text=True).stdout.split()
        ours = sorted(f for f in tracked
                      if not f.startswith(("thirdparty/", "raymob/", "tests/fixtures/"))
                      and not f.endswith("_impl.c"))
        for must in ("tools/rres_pack.c", "tools/md5.c", "examples/plain_c/src/main.c"):
            self.assertIn(must, ours)
        listed = self.listed()
        for path in ours:
            with self.subTest(path=path):
                self.assertIn(path, listed)

    def test_the_tools_keep_the_analyzer(self):
        """tools/.clang-tidy inherits the root file and takes two checks off;
        the analyzer is not one of them. Seen with a leak written in tools/."""
        import shutil
        import subprocess
        tidy = shutil.which("clang-tidy")
        if tidy is None:
            if os.environ.get("RMP_REQUIRE_CLANG_TIDY") == "1":
                self.fail("clang-tidy is required here (RMP_REQUIRE_CLANG_TIDY=1)")
            self.skipTest("clang-tidy not installed")
        tmp = Path(tempfile.mkdtemp(prefix=".tidy-probe-", dir=REPO / "tools"))
        try:
            probe = tmp / "probe.c"
            probe.write_text("#include <stdlib.h>\n#include <string.h>\n"
                             "int leaks(const char *s) {\n"
                             "    char *copyOf = malloc(8);\n"
                             "    if (!copyOf) return 0;\n"
                             "    memcpy(copyOf, s, 8);\n"
                             "    return copyOf[0];\n}\n")
            got = subprocess.run([tidy, "--quiet", str(probe), "--", "-std=c11"],
                                 capture_output=True, text=True)
            said = got.stdout + got.stderr
        finally:
            shutil.rmtree(tmp)
        self.assertIn("Potential leak of memory pointed to by 'copyOf'", said)
        self.assertNotIn("readability-identifier-naming", said)
        self.assertNotIn("DeprecatedOrUnsafeBufferHandling", said)


class WorkflowSecretsInheritTest(unittest.TestCase):
    """A callee that reads `secrets.*` needs a caller that passes them.

    ci.yml's own header calls this failure mode #2: "Called workflows get no
    secrets unless you say `secrets: inherit`. Miss it and the signing / butler
    / GCP steps all quietly take their 'not configured, skipping' branch and
    the pipeline looks green." Nothing checked it -- the tree happened to be
    right, which is the state immediately before a regression.

    Run against a fixture rather than only against the tree, because a checker
    nobody has watched go red is a hope with a name.
    """

    CALLEE = """
name: Callee
on:
  workflow_call:
    inputs:
      who:
        required: true
        type: string
jobs:
  sign:
    runs-on: ubuntu-24.04
    steps:
      - run: echo "${{ secrets.ANDROID_KEYSTORE_BASE64 }}" > key
"""

    CALLER = """
name: Caller
on: [push]
jobs:
  android:
    uses: ./.github/workflows/_callee.yml
    with:
      who: me
%s
"""

    def run_check(self, caller_tail):
        import subprocess, tempfile
        with tempfile.TemporaryDirectory() as root:
            flows = Path(root) / ".github" / "workflows"
            flows.mkdir(parents=True)
            (flows / "_callee.yml").write_text(self.CALLEE)
            (flows / "caller.yml").write_text(self.CALLER % caller_tail)
            return subprocess.run(
                ["bash", str(REPO / "tools" / "workflow_check.sh"), root],
                capture_output=True, text=True)

    def test_a_caller_that_forgets_secrets_inherit_is_red(self):
        got = self.run_check("")
        if "skip  PyYAML" in got.stdout:
            self.skipTest("PyYAML not installed")
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("without `secrets: inherit`", got.stdout)
        self.assertIn("ANDROID_KEYSTORE_BASE64", got.stdout)

    def test_a_caller_that_inherits_is_green(self):
        got = self.run_check("    secrets: inherit")
        if "skip  PyYAML" in got.stdout:
            self.skipTest("PyYAML not installed")
        self.assertEqual(got.returncode, 0, got.stdout)

    def test_the_real_tree_passes(self):
        import subprocess
        got = subprocess.run(["bash", str(REPO / "tools" / "workflow_check.sh")],
                             capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stdout)

    def test_the_pyyaml_skip_is_a_failure_inside_the_image(self):
        """The branch that made this check a no-op in CI for months. The image
        has no reason to lose python3-yaml again, but the difference between a
        skip and a failure is the difference between a gate and a habit."""
        text = (REPO / "tools" / "workflow_check.sh").read_text()
        self.assertIn("/etc/raylib-build-image.json", text)
        self.assertIn("FAIL: PyYAML is missing INSIDE the build image", text)


class PwshStepEndsWithZeroTest(unittest.TestCase):
    """A pwsh step ends with the last native exit code -- the runner appends
    `exit $LASTEXITCODE` -- so a step that checks a command fails the way it
    should prints PASS and fails. The Windows rmp job did, on its first run."""

    FLOW = """
name: Flow
on: [push]
jobs:
  win:
    runs-on: windows-latest
    defaults:
      run:
        shell: pwsh
    steps:
      - name: A typo is a usage error
        run: |
          & ./rmp.ps1 no-such-command
          if ($LASTEXITCODE -ne 2) { throw "not 2" }
          Write-Host "PASS"
%s
"""

    def run_check(self, tail):
        import subprocess, tempfile
        with tempfile.TemporaryDirectory() as root:
            flows = Path(root) / ".github" / "workflows"
            flows.mkdir(parents=True)
            (flows / "flow.yml").write_text(self.FLOW % tail)
            got = subprocess.run(["bash", str(REPO / "tools" / "workflow_check.sh"), root],
                                 capture_output=True, text=True)
        if "skip  PyYAML" in got.stdout:
            self.skipTest("PyYAML not installed")
        return got

    def test_without_exit_0_it_is_red(self):
        got = self.run_check("")
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("A typo is a usage error expects a command to exit non-zero", got.stdout)

    def test_with_exit_0_it_is_green(self):
        got = self.run_check("          # the 2 above is not this step's answer\n"
                             "          exit 0")
        self.assertEqual(got.returncode, 0, got.stdout)
        self.assertIn("1 pwsh step(s) that expect a failure end with exit 0", got.stdout)


class ThisFileRunsWholeTest(unittest.TestCase):
    """`python3 tests/configure_test.py` used to run 108 of these tests.

    The `if __name__ == "__main__"` block sat in the middle of the file, and
    every class defined after it was never collected when the file was run
    directly -- the same shape as the 105-test suite that quietly ran 72.
    `rmp test` uses `unittest discover`, which does not care; a human typing
    the file name does. The block is the last statement now, and this keeps it
    there.
    """

    def test_main_is_the_last_statement(self):
        text = Path(__file__).read_text().rstrip()
        self.assertTrue(text.endswith('if __name__ == "__main__":\n    unittest.main(verbosity=2)'),
                        "something was added after the __main__ block; move the block back "
                        "to the end of the file")


class ConfigureAudioTest(unittest.TestCase):
    """[audio] master / music / sfx -- the volumes rmp::audio starts at.

    Each is a fraction of full volume. The mistakes worth catching at the line
    rather than by ear: a value above 1 (it is not "louder", it is clipping, and
    rmp::audio would clamp it silently), a negative, a string, and `true` --
    which Python counts as the integer 1 and would sail through as full volume.
    """

    def audio(self, **overrides):
        return base_config(audio=dict(copy.deepcopy(cfgmod.DEFAULTS["audio"]), **overrides))

    def test_the_defaults_leave_headroom_for_the_music(self):
        self.assertEqual(cfgmod.DEFAULTS["audio"], {"master": 1.0, "music": 0.8, "sfx": 1.0})

    def test_every_value_a_slider_can_produce_is_accepted(self):
        for key in ("master", "music", "sfx"):
            for value in (0, 0.0, 0.25, 0.5, 1, 1.0):
                with self.subTest(key=key, value=value), quiet():
                    cfgmod.validate(self.audio(**{key: value}), False)

    def test_out_of_range_is_refused_and_says_it_is_a_fraction(self):
        for key in ("master", "music", "sfx"):
            for bad in (-0.1, -1, 1.01, 2, 100):
                with self.subTest(key=key, value=bad):
                    with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                        cfgmod.validate(self.audio(**{key: bad}), False)
                    self.assertIn("between 0 and 1", str(caught.exception))
                    self.assertIn(key, str(caught.exception))

    def test_nan_is_refused(self):
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(self.audio(sfx=float("nan")), False)

    def test_it_has_to_be_a_number_and_true_is_not_one(self):
        for key in ("master", "music", "sfx"):
            for bad in (True, False, "0.5", None, [0.5], {"v": 1}):
                with self.subTest(key=key, value=bad):
                    with self.assertRaises(cfgmod.ConfigError), quiet():
                        cfgmod.validate(self.audio(**{key: bad}), False)

    def test_the_error_points_at_the_line(self):
        """The rule: an invalid .toml fails in configure.py and says WHERE --
        the key, so the reporter can put the cursor on the exact line."""
        for key in ("master", "music", "sfx"):
            with self.subTest(key=key):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.audio(**{key: 3}), False)
                self.assertEqual(caught.exception.where, ("audio", key))
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(self.audio(**{key: "x"}), False)
                self.assertEqual(caught.exception.where, ("audio", key))

    def test_the_values_reach_the_generated_header(self):
        """A volume nothing reads is a volume that does nothing -- and a
        generator that wrote music's value into MASTER passed the old version
        of this test, which only looked for the macro names. Three different
        values, each read back from the header configure.py would write."""
        cfg = self.audio(master=0.125, music=0.25, sfx=0.5)
        with quiet():
            cfgmod.validate(cfg, False)
        with generated_header(cfg) as text:
            self.assertRegex(text, r"#define RMP_AUDIO_MASTER\s+0\.125f\n")
            self.assertRegex(text, r"#define RMP_AUDIO_MUSIC\s+0\.25f\n")
            self.assertRegex(text, r"#define RMP_AUDIO_SFX\s+0\.5f\n")
        audio_cpp = (REPO / "src" / "rmp" / "audio.cpp").read_text()
        for define in ("RMP_AUDIO_MASTER", "RMP_AUDIO_MUSIC", "RMP_AUDIO_SFX"):
            with self.subTest(define=define):
                self.assertIn(define, audio_cpp)



class RaudioStubCoverageTest(unittest.TestCase):
    """`disabled_modules = ["raudio"]` links, because every raudio function the
    framework calls has a stub.

    The stub list used to be kept by memory, and memory had already missed
    UnloadSound (the resource table has called it since phase 3; GNU ld's
    section GC hid it, Apple's ld64 would not have). rmp::audio then made the
    calls reachable from every game, so a missing stub is a link error on every
    linker. This scans the sources for calls to anything raylib.h declares
    under "(Module: audio)" and requires a definition for each in the stub.
    """

    RAYLIB_H = REPO / "thirdparty" / "raylib" / "src" / "raylib.h"

    def audio_functions(self) -> set[str]:
        text = self.RAYLIB_H.read_text(encoding="utf-8")
        start = text.index("(Module: audio)")
        end = text.index("#if defined(__cplusplus)", start)
        block = re.sub(r"//[^\n]*", "", text[start:end])
        names = set()
        for ret in re.findall(r"RLAPI\s+([^;(]+?)\s*\(", block):
            names.add(re.findall(r"(\w+)$", ret)[0])
        return names

    @staticmethod
    def code_only(text: str) -> str:
        """The text with comments and string literals blanked out, so a name
        mentioned in a log message or a comment is not a reference."""
        return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                      " ", text, flags=re.S)

    def called(self, names: set[str]) -> dict[str, list[str]]:
        # Every REFERENCE, not only calls: `&SetMusicPitch` stored in a pointer
        # needs a stub just as much, and the first version of this gate, which
        # looked for `Name(`, passed while that link failed. And every source
        # the rmp library compiles, .c included.
        sources = sorted(p for p in (REPO / "src" / "rmp").rglob("*")
                         if p.suffix in (".cpp", ".c", ".h")) + \
            [REPO / "thirdparty" / "rres" / "rres-raylib.h"]
        found: dict[str, list[str]] = {}
        for path in sources:
            text = self.code_only(path.read_text(encoding="utf-8"))
            for name in names:
                if re.search(r"(?<![\w.>])%s\b" % name, text):
                    found.setdefault(name, []).append(str(path.relative_to(REPO)))
        return found

    def test_a_reference_that_is_not_a_call_is_seen(self):
        names = {"SetMusicPitch", "PlaySound"}
        text = self.code_only('auto f = &SetMusicPitch; // PlaySound( in a comment\n'
                              'log("PlaySound(x)"); /* PlaySound( */')
        self.assertRegex(text, r"(?<![\w.>])SetMusicPitch\b")
        self.assertNotIn("PlaySound", text)
        del names

    def stubbed(self) -> set[str]:
        # A definition starts a line: return type, name, parameters, then the
        # body's brace on the same line or the next.
        body = cfgmod.STUB_SOURCE.format(header="x")
        return set(re.findall(r"^[A-Za-z]\w*\s+\*?(\w+)\([^;{]*\)\s*\{", body, re.M))

    def test_the_scan_sees_the_audio_module(self):
        # A regex that matched nothing would pass everything below.
        names = self.audio_functions()
        self.assertGreater(len(names), 50)
        for name in ("InitAudioDevice", "PlaySound", "UpdateMusicStream", "UnloadSound"):
            self.assertIn(name, names)
        self.assertNotIn("DrawTexture", names)

    def test_the_scan_sees_the_framework_calling_it(self):
        found = self.called(self.audio_functions())
        # rmp::audio and the resource table, at least; if these vanish the
        # scan broke, not the framework.
        self.assertIn("InitAudioDevice", found)
        self.assertIn("src/rmp/resource.cpp", found.get("UnloadSound", []))
        self.assertIn("thirdparty/rres/rres-raylib.h", found.get("LoadWaveFromMemory", []))

    def test_every_called_audio_function_has_a_stub(self):
        found = self.called(self.audio_functions())
        missing = {n: where for n, where in found.items() if n not in self.stubbed()}
        self.assertEqual(missing, {}, "called by the framework but not stubbed in "
                         "STUB_SOURCE in tools/configure.py -- disabled_modules = "
                         "[\"raudio\"] would not link")

    def test_the_stub_parser_is_not_blind(self):
        stubbed = self.stubbed()
        self.assertIn("IsAudioDeviceReady", stubbed)
        self.assertIn("LoadWaveFromMemory", stubbed)
        self.assertIn("LoadMusicStreamFromMemory", stubbed)

    def test_no_device_is_what_the_stub_says(self):
        # The one stub with behaviour: rmp::audio asks it once and goes quiet.
        self.assertIn("bool IsAudioDeviceReady(void) { return false; }",
                      cfgmod.STUB_SOURCE.format(header="x"))

    def test_the_stub_compiles_against_the_real_raylib_h(self):
        # A stub with the wrong signature is a conflicting-types error in the
        # one build that uses it, which nobody runs until they need it.
        import shutil
        import subprocess
        import tempfile
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("no C compiler on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "module_stubs.c"
            src.write_text(cfgmod.STUB_SOURCE.format(header="test"), encoding="utf-8")
            result = subprocess.run(
                [cc, "-std=c99", "-Wall", "-Werror", "-fsyntax-only",
                 "-I", str(self.RAYLIB_H.parent), str(src)],
                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)



class ConfigureTableShapeTest(unittest.TestCase):
    """A value where a table goes is a rejection with a line, not a traceback.

    `audio = 0.5` above every section, or `admob = true` inside [android], used
    to be stored as it was, and the first check to index it died with
    "TypeError: 'float' object is not subscriptable". CLAUDE.md: the wrong TYPE
    is one of the cases every option is tested with.
    """

    def test_every_table_refuses_a_scalar(self):
        tables = [k for k, v in cfgmod.DEFAULTS.items() if isinstance(v, dict)]
        self.assertGreater(len(tables), 10)
        for table in tables:
            for bad in (0.5, 1, True, "text", [1, 2]):
                with self.subTest(table=table, value=bad):
                    with self.assertRaises(cfgmod.ConfigError) as caught:
                        cfgmod.deep_merge(cfgmod.DEFAULTS, {table: bad})
                    self.assertIn(f"[{table}] has to be a table", str(caught.exception))
                    self.assertEqual(caught.exception.where, ("", table))

    def test_a_nested_table_refuses_a_scalar_too(self):
        nested = [(t, k) for t, v in cfgmod.DEFAULTS.items() if isinstance(v, dict)
                  for k, w in v.items() if isinstance(w, dict)]
        self.assertTrue(nested, "no nested table left to test")
        for table, key in nested:
            with self.subTest(table=table, key=key):
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    cfgmod.deep_merge(cfgmod.DEFAULTS, {table: {key: True}})
                self.assertIn(f"[{table}.{key}] has to be a table", str(caught.exception))
                self.assertEqual(caught.exception.where, (table, key))

    def test_the_line_is_found_at_the_top_level_and_inside_a_section(self):
        import tempfile
        original = cfgmod.TOML
        with tempfile.TemporaryDirectory() as tmp:
            cfgmod.TOML = Path(tmp) / "raylib_multiplatform.toml"
            try:
                cfgmod.TOML.write_text('# a comment\naudio = 0.5\n\n[input]\ndeadzone = 0.2\n')
                self.assertEqual(cfgmod.locate("", "audio"), (2, "audio = 0.5"))
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    cfgmod.load_config()
                self.assertEqual(cfgmod.locate_from(caught.exception), (2, "audio = 0.5"))
                # And an unknown key names its own line, not only its name.
                cfgmod.TOML.write_text('[audio]\nmaster = 1.0\nmastr = 0.5\n')
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    cfgmod.load_config()
                self.assertEqual(cfgmod.locate_from(caught.exception), (3, "mastr = 0.5"))
            finally:
                cfgmod.TOML = original
class ConfigureSaveTest(unittest.TestCase):
    """[save] portable / encrypt / version -- where rmp::save writes, whether
    it seals by default, and the version of the game's own save format.

    Two switches and a counter. The mistakes worth catching at the line: a
    string where a bool goes ("true" in quotes), `1` for a switch, a version of
    0 or a negative, a float version, and `true` as a version -- which Python
    counts as the integer 1 and would pass as version 1.
    """

    def save(self, **overrides):
        return base_config(save=dict(copy.deepcopy(cfgmod.DEFAULTS["save"]), **overrides))

    def reject(self, **overrides):
        with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
            cfgmod.validate(self.save(**overrides), False)
        return caught.exception

    def test_the_defaults(self):
        self.assertEqual(cfgmod.DEFAULTS["save"],
                         {"portable": False, "encrypt": False, "version": 1})

    def test_every_valid_combination_is_accepted(self):
        for portable in (False, True):
            for encrypt in (False, True):
                for version in (1, 2, 17, 2_000_000_000):
                    with self.subTest(portable=portable, encrypt=encrypt, version=version), quiet():
                        cfgmod.validate(self.save(portable=portable, encrypt=encrypt,
                                                  version=version), False)

    def test_the_switches_are_bools_and_nothing_else(self):
        for key in ("portable", "encrypt"):
            for bad in ("true", "false", 1, 0, None, [True], {"on": True}, 1.0):
                with self.subTest(key=key, value=bad):
                    error = self.reject(**{key: bad})
                    self.assertIn(f"[save] {key}", str(error))
                    self.assertIn("true or false", str(error))
                    self.assertEqual(error.where, ("save", key))

    def test_the_version_is_a_whole_number(self):
        for bad in (True, False, "1", 1.0, 1.5, None, [1], {"v": 1}):
            with self.subTest(value=bad):
                error = self.reject(version=bad)
                self.assertIn("[save] version", str(error))
                self.assertIn("whole number", str(error))
                self.assertEqual(error.where, ("save", "version"))

    def test_the_version_starts_at_one_and_fits_an_int(self):
        for bad in (0, -1, -2_000_000_000, 2_000_000_001, 2**31, 2**63):
            with self.subTest(value=bad):
                error = self.reject(version=bad)
                self.assertIn("between 1 and 2000000000", str(error))
                self.assertEqual(error.where, ("save", "version"))

    def test_an_unknown_key_is_refused(self):
        # A typo ("portble") must not be a switch that silently stays off.
        with self.assertRaises(cfgmod.ConfigError) as caught:
            cfgmod.deep_merge(cfgmod.DEFAULTS, {"save": {"portble": True}})
        self.assertIn("[save.portble]", str(caught.exception))

    def test_the_values_reach_the_generated_header(self):
        """A switch nothing reads is a switch that does nothing -- so read the
        header configure.py would write, not the source that writes it."""
        for portable, encrypt, version in ((False, False, 1), (True, True, 42)):
            with self.subTest(portable=portable, encrypt=encrypt, version=version):
                cfg = self.save(portable=portable, encrypt=encrypt, version=version)
                with quiet():
                    cfgmod.validate(cfg, False)
                with generated_header(cfg) as text:
                    self.assertRegex(text, rf"#define RMP_SAVE_PORTABLE\s+{int(portable)}\n")
                    self.assertRegex(text, rf"#define RMP_SAVE_ENCRYPT\s+{int(encrypt)}\n")
                    self.assertRegex(text, rf"#define RMP_SAVE_VERSION\s+{version}\n")

    def test_the_toml_documents_every_key(self):
        toml = (REPO / "raylib_multiplatform.toml").read_text()
        section = toml[toml.index("[save]"):toml.index("[ui]")]
        for key in ("portable", "encrypt", "version"):
            with self.subTest(key=key):
                self.assertRegex(section, rf"(?m)^{key}\s*=")
        # Omar's rule, in the one section where a password would look at home.
        self.assertNotRegex(section.lower(), r"(?m)^(password|key|secret)\s*=")



class WebSavesLinkTest(unittest.TestCase):
    """The web half of rmp::save is three things in the link line, and losing
    any of them loses every save on reload while the game boots green.

    -lidbfs.js provides IDBFS; cmake/generated/rmp_web_name.js hands the
    game's name to cmake/web/rmp_web.js, which must come after it and mounts
    IndexedDB at /rmp_save/<name>; src/rmp/save.cpp must look in that same
    folder. The browser boot test catches a failing mount at run time
    (.github/scripts/web_boot_test.js turns any "rmp::save:" warning into an
    error); this catches the link line before anything is built.
    """

    def link_flags(self) -> str:
        cmake = (REPO / "CMakeLists.txt").read_text()
        start = cmake.index('LINK_FLAGS "-s TOTAL_MEMORY=')
        return cmake[start:cmake.index('")', start)]

    def test_idbfs_and_both_pre_js_are_linked_in_order(self):
        flags = self.link_flags()
        self.assertIn("-lidbfs.js", flags)
        name_js = flags.find("--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/rmp_web_name.js")
        web_js = flags.find("--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/cmake/web/rmp_web.js")
        self.assertGreaterEqual(name_js, 0)
        self.assertGreaterEqual(web_js, 0)
        self.assertLess(name_js, web_js, "the name has to be set before rmp_web.js reads it")

    def test_the_mount_and_the_folder_agree(self):
        web = (REPO / "cmake" / "web" / "rmp_web.js").read_text()
        self.assertIn("'/rmp_save/' + (Module['rmpSaveName']", web)
        self.assertIn("FS.mount(IDBFS, {}, dir)", web)
        save = (REPO / "src" / "rmp" / "save.cpp").read_text()
        self.assertIn('return "/rmp_save/" RMP_PROJECT_NAME "/";', save)

    def test_the_generated_name_is_the_project_name_as_a_js_string(self):
        captured = {}
        original = cfgmod.write
        cfgmod.write = lambda path, content: captured.__setitem__(Path(path).name, content)
        try:
            for name in ("ray_test", 'quo"te', "back\\slash"):
                with self.subTest(name=name):
                    captured.clear()
                    cfg = base_config()
                    cfg["project"]["name"] = name
                    cfgmod.gen_cmake(cfg)
                    js = captured["rmp_web_name.js"]
                    self.assertIn(f"Module['rmpSaveName'] = {json.dumps(name)};", js)
        finally:
            cfgmod.write = original

    def test_the_boot_test_fails_on_a_save_warning(self):
        boot = (REPO / ".github" / "scripts" / "web_boot_test.js").read_text()
        at = boot.index("/^rmp::save:/.test(text)")
        # And before the line that drops everything that is not an error.
        self.assertLess(at, boot.index("if (msg.type() !== 'error') return;"))



class OwnHeaderIncludeTest(unittest.TestCase):
    """CLAUDE.md: "Our headers do not include each other." Where a type by
    value makes it unavoidable, the header includes the other one and says why
    -- on the include's own line, which is what this checks, so an include
    added without a reason fails here instead of quietly dragging a module
    into every file that touches this one. And no cycles: two of ours that
    need each other means one of them should not exist.

    next_architecture/02-headers.md said a test compared this; none did until
    the phase 11-12 completeness review found the claim unbacked.
    """

    INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]rmp/([a-z_]+\.h)[>"](.*)$')

    def own_includes(self) -> dict[str, list[tuple[int, str, str]]]:
        found: dict[str, list[tuple[int, str, str]]] = {}
        for header in sorted((REPO / "include" / "rmp").glob("*.h")):
            for n, line in enumerate(header.read_text().splitlines(), start=1):
                m = self.INCLUDE.match(line)
                if m and m.group(1) != "config.h":
                    found.setdefault(header.name, []).append((n, m.group(1), m.group(2)))
        return found

    def test_the_scan_sees_the_known_ones(self):
        found = self.own_includes()
        self.assertIn("object.h", [inc for _, inc, _ in found.get("scene.h", [])])
        self.assertIn("assets.h", [inc for _, inc, _ in found.get("object.h", [])])

    def test_every_include_of_another_rmp_header_says_why_on_its_line(self):
        unexplained = [f"{h}:{n} includes rmp/{inc} without a // reason"
                       for h, rows in self.own_includes().items()
                       for n, inc, rest in rows
                       if not re.match(r"\s*//\s*\S", rest)]
        self.assertEqual(unexplained, [])

    def test_no_two_of_ours_include_each_other(self):
        graph = {h: {inc for _, inc, _ in rows} for h, rows in self.own_includes().items()}

        def reaches(start: str, goal: str, seen: set[str]) -> bool:
            for nxt in graph.get(start, ()):
                if nxt == goal or (nxt not in seen and reaches(nxt, goal, seen | {nxt})):
                    return True
            return False

        cycles = sorted(h for h in graph if reaches(h, h, {h}))
        self.assertEqual(cycles, [])

    def test_the_new_modules_stand_alone(self):
        # audio.h and save.h include nothing of ours but config.h.
        found = self.own_includes()
        self.assertNotIn("audio.h", found)
        self.assertNotIn("save.h", found)

    def test_config_h_is_one_include_of_a_file_that_includes_nothing(self):
        """What rmp/config.h and rmp/app.h say config.h costs: one include, of
        the generated file of #defines, which includes nothing. They said "no
        includes of its own", over the one include config.h has."""
        text = (REPO / "include" / "rmp" / "config.h").read_text()
        includes = re.findall(r"(?m)^\s*#\s*include\s*(\S+)", text)
        self.assertEqual(includes, ["<rmp/generated/config.h>"])
        with generated_header(base_config()) as header:
            self.assertNotRegex(header, r"(?m)^\s*#\s*include")
        for name in ("config.h", "app.h"):
            with self.subTest(header=name):
                prose = re.sub(r"\s*\n//\s*", " ", (REPO / "include" / "rmp" / name).read_text())
                self.assertNotIn("no includes of its own", prose)



class ExampleSoundsExistTest(unittest.TestCase):
    """Every sound an example asks rmp::audio for by name is in its resources/.

    Sounds load the first time they play, so a missing hit.wav is invisible to
    the examples job: thirty frames of Pong never reach a paddle, the game
    boots with assets_failed=0, and the first player to return a serve hears
    nothing. The names are looked up the way rmp::audio looks them up.
    """

    CALL = re.compile(r'rmp::audio::(?:play|music)\(\s*"([^"]+)"')
    SEARCH = (".wav", ".ogg", ".mp3", ".qoa")
    KNOWN = SEARCH + (".flac", ".xm", ".mod")

    def candidates(self, name: str) -> list[str]:
        file = re.split(r"[/\\]", name)[-1]
        dot = file.rfind(".")
        if dot > 0 and file[dot:].lower() in self.KNOWN:
            return [name]
        return [name + ext for ext in self.SEARCH]

    def calls(self) -> list[tuple[Path, str]]:
        found = []
        for src in sorted((REPO / "examples").rglob("*.cpp")):
            # Comments out, strings kept: a comment that mentions
            # music("song") is prose, not a sound the game asks for.
            code = re.sub(r"//[^\n]*|/\*.*?\*/", " ", src.read_text(), flags=re.S)
            for name in self.CALL.findall(code):
                found.append((src, name))
        return found

    def resources_of(self, src: Path) -> Path:
        # examples/<area>/<name>/src/... -> examples/<area>/<name>/resources,
        # else the game's own resources/, as rmp_add_game() decides.
        for parent in src.parents:
            if parent.name == "src" and (parent.parent / "resources").is_dir():
                return parent.parent / "resources"
        return REPO / "resources"

    def test_the_scan_finds_the_sounds_the_games_play(self):
        names = {name for _, name in self.calls()}
        self.assertTrue({"hit", "point", "win"} <= names, names)

    def test_pongs_sounds_are_what_the_generator_makes(self):
        # License-sounds.txt says they were synthesized; this says by what,
        # and that the committed bytes are exactly its output.
        spec = importlib.util.spec_from_file_location(
            "make_example_art", REPO / "tools" / "make_example_art.py")
        art = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(art)
        made = art.pong_sounds()
        self.assertEqual(sorted(made), ["hit.wav", "point.wav", "win.wav"])
        for name, data in made.items():
            with self.subTest(sound=name):
                committed = (REPO / "examples" / "games" / "01_pong" / "resources" / name).read_bytes()
                self.assertEqual(data, committed)

    def test_every_named_sound_resolves_to_a_file(self):
        missing = [f"{src.relative_to(REPO)}: \"{name}\" (looked for "
                   f"{', '.join(self.candidates(name))} in {self.resources_of(src).relative_to(REPO)})"
                   for src, name in self.calls()
                   if not any((self.resources_of(src) / c).is_file() for c in self.candidates(name))]
        self.assertEqual(missing, [])


# ---------------------------------------------------------------------------
# [ci] on_push: the triggers of ci.yml, generated
# ---------------------------------------------------------------------------

CI_YML = REPO / ".github" / "workflows" / "ci.yml"


@contextlib.contextmanager
def ci_project(on_push=None, ci_text=None, crlf=False):
    """A scratch project for [ci] on_push: its .toml and its ci.yml, with
    configure.py's REPO and TOML pointed at it. `on_push=None` leaves [ci] out
    of the .toml; `ci_text=None` copies the framework's ci.yml, "" writes none.
    Yields the ci.yml's path."""
    original_toml = cfgmod.TOML
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        toml = root / "raylib_multiplatform.toml"
        toml.write_text('[project]\nname = "demo"\n'
                        + ("" if on_push is None else
                           f"\n[ci]\n# a comment above\non_push = {str(on_push).lower()}\n"))
        ci = root / ".github" / "workflows" / "ci.yml"
        if ci_text != "":
            ci.parent.mkdir(parents=True)
            text = CI_YML.read_text() if ci_text is None else ci_text
            ci.write_bytes(text.replace("\n", "\r\n" if crlf else "\n").encode())
        cfgmod.TOML = toml
        try:
            with repo_at(root):
                yield ci
        finally:
            cfgmod.TOML = original_toml


def ci_config(on_push: bool) -> dict:
    return base_config(ci={"on_push": on_push})


def outside_the_block(text: str) -> list[str]:
    """ci.yml's lines with the generated block taken out."""
    lines = text.splitlines()
    begin = next(i for i, line in enumerate(lines) if line.strip().startswith(cfgmod.CI_BEGIN))
    end = next(i for i, line in enumerate(lines) if line.strip().startswith(cfgmod.CI_END))
    return lines[:begin] + lines[end + 1:]


@contextlib.contextmanager
def environment(**values):
    """os.environ with these set, and None meaning unset, for one block."""
    saved = {k: os.environ.get(k) for k in values}
    try:
        for k, v in values.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        yield
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


class CiOnPushTest(unittest.TestCase):
    """[ci] on_push writes the push and pull_request triggers of ci.yml, and
    nothing else in it. ci.yml is the framework's and every game's file; a
    game that turns CI off on push differs from the framework between the two
    markers and nowhere else, and a ci.yml that disagrees with the .toml is
    refused at the .toml's line -- by --check, by every --print-* (the CI
    config job's first call is one) and by any run inside GitHub Actions --
    because the run it starts is not the one the .toml asks for."""

    def test_the_framework_runs_ci_on_every_push(self):
        cfg = cfgmod.load_config()
        self.assertIs(cfg["ci"]["on_push"], True)
        self.assertIn("\n[ci]\n", (REPO / "raylib_multiplatform.toml").read_text())
        self.assertRegex(toml_section("ci"), r"(?m)^on_push = true$")
        text = CI_YML.read_text()
        self.assertEqual(text.count(cfgmod.ci_trigger_block(cfg)), 1)
        with quiet():
            self.assertFalse(cfgmod.sync_ci_triggers(cfg, write_it=False))

    def triggers(self, on_push: bool) -> dict:
        yaml = require_yaml(self)
        text = CI_YML.read_text()
        lines = text.splitlines(keepends=True)
        begin = next(i for i, line in enumerate(lines) if cfgmod.CI_BEGIN in line)
        end = next(i for i, line in enumerate(lines) if cfgmod.CI_END in line)
        made = ("".join(lines[:begin]) + cfgmod.ci_trigger_block(ci_config(on_push))
                + "".join(lines[end + 1:]))
        data = yaml.safe_load(made)
        return data.get("on", data.get(True))   # YAML 1.1 reads `on` as True

    def test_both_variants_are_the_triggers_they_say(self):
        on = self.triggers(True)
        self.assertEqual(on["push"], {"branches": ["main"], "tags": ["v*"],
                                      "paths-ignore": ["**.md", "LICENSE"]})
        self.assertEqual(on["pull_request"], {"paths-ignore": ["**.md", "LICENSE"]})
        off = self.triggers(False)
        # Tags only: GitHub starts nothing for a push to a branch, and a tag
        # still releases. No pull_request at all.
        self.assertEqual(off["push"], {"tags": ["v*"]})
        self.assertNotIn("pull_request", off)
        self.assertNotIn("branches", off["push"])
        # workflow_dispatch is hand-written, and the same either way.
        self.assertEqual(off["workflow_dispatch"], on["workflow_dispatch"])
        self.assertEqual(set(on), {"push", "pull_request", "workflow_dispatch"})
        self.assertEqual(set(off), {"push", "workflow_dispatch"})

    def test_the_header_line_the_docs_pin_is_still_there(self):
        """rmp-docs' testing page greps for this line of ci.yml, byte for byte."""
        self.assertEqual(CI_YML.read_text().splitlines()[2],
                         "#   push / pull_request  ->  fast lane: Linux x64, Web, Android, "
                         "Windows x64")
        self.assertIn("[ci] on_push = false", CI_YML.read_text().splitlines()[3])

    def test_generation_writes_once_and_only_the_block(self):
        with ci_project(on_push=False) as ci, quiet(), contextlib.redirect_stdout(io.StringIO()):
            before = ci.read_text()
            self.assertTrue(cfgmod.sync_ci_triggers(ci_config(False), write_it=True))
            after = ci.read_text()
            self.assertIn(cfgmod.ci_trigger_block(ci_config(False)), after)
            self.assertNotEqual(after, before)
            self.assertEqual(outside_the_block(after), outside_the_block(before))
            # A second run has nothing to do, and touches nothing.
            stamp = ci.stat().st_mtime_ns
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(False), write_it=True))
            self.assertEqual(ci.stat().st_mtime_ns, stamp)
            # Comparing now agrees, and back to true is the framework's file again.
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(False), write_it=False))
            self.assertTrue(cfgmod.sync_ci_triggers(ci_config(True), write_it=True))
            self.assertEqual(ci.read_text(), CI_YML.read_text())

    def test_a_ci_yml_that_disagrees_is_refused_at_the_line(self):
        for on_push in (False, True):
            text = CI_YML.read_text()
            if on_push:   # the .toml says true; the file says false
                text = text.replace(cfgmod.ci_trigger_block(ci_config(True)),
                                    cfgmod.ci_trigger_block(ci_config(False)))
            with self.subTest(on_push=on_push), ci_project(on_push=on_push, ci_text=text) as ci:
                with self.assertRaises(cfgmod.ConfigError) as caught:
                    cfgmod.sync_ci_triggers(ci_config(on_push), write_it=False)
                message = str(caught.exception)
                self.assertIn(f"[ci] on_push = {str(on_push).lower()}", message)
                self.assertIn(".github/workflows/ci.yml", message)
                self.assertIn("commit", message)
                self.assertEqual(cfgmod.locate_from(caught.exception),
                                 (6, f"on_push = {str(on_push).lower()}"))
                self.assertEqual(ci.read_text(), text, "a refusal wrote the file")

    def test_check_every_print_and_ci_refuse_and_write_nothing(self):
        """The CI config job calls --print-name first, and rmp deploy
        --print-config: both stop. In GitHub Actions even a plain run only
        compares, so every leaf job's configure step stops too."""
        cases = [["--check"], ["--print-name"], ["--print-targets"], ["--print-ci"],
                 ["--print-defines"], ["--print-pins"], ["--print-config"]]
        with ci_project(on_push=False) as ci:
            before = ci.read_bytes()
            for argv in cases:
                with self.subTest(argv=argv), environment(GITHUB_ACTIONS=None):
                    _, caught = run_cli(*argv)
                    self.assertIsInstance(caught, cfgmod.ConfigError)
                    self.assertIn("[ci] on_push", str(caught))
            with environment(GITHUB_ACTIONS="true"):
                _, caught = run_cli()
                self.assertIsInstance(caught, cfgmod.ConfigError)
                self.assertIn("[ci] on_push", str(caught))
            self.assertEqual(ci.read_bytes(), before)

    def test_generating_on_a_machine_writes_it(self):
        """The plain run outside CI: what `cmake --preset`, rmp build and rmp
        run do. The other generators are stood in for -- the scratch project
        has nothing for them to read -- so this is main()'s choice to write,
        and NewTest's game is the run with every generator for real."""
        stubs = {name: (lambda *a, **k: False) for name in (
            "gen_cmake", "gen_app_config", "gen_licenses", "gen_gradle_properties",
            "gen_android_manifest", "generate_icons", "gen_ios_project")}
        saved = {name: getattr(cfgmod, name) for name in stubs}
        saved_stamp = cfgmod.STAMP
        with ci_project(on_push=False) as ci:
            cfgmod.STAMP = ci.parent / "config.stamp"
            for name, stub in stubs.items():
                setattr(cfgmod, name, stub)
            try:
                with environment(GITHUB_ACTIONS=None):
                    out, caught = run_cli()
            finally:
                for name, original in saved.items():
                    setattr(cfgmod, name, original)
                cfgmod.STAMP = saved_stamp
            self.assertIsNone(caught)
            self.assertIn("rewrote the triggers", out)
            self.assertIn(cfgmod.ci_trigger_block(ci_config(False)), ci.read_text())

    def test_the_markers_missing_doubled_or_reversed_are_refused(self):
        text = CI_YML.read_text()
        begin_line = next(line for line in text.splitlines(keepends=True)
                          if cfgmod.CI_BEGIN in line)
        end_line = next(line for line in text.splitlines(keepends=True)
                        if cfgmod.CI_END in line)
        broken = {
            "no begin": text.replace(begin_line, ""),
            "no end": text.replace(end_line, ""),
            "neither": text.replace(begin_line, "").replace(end_line, ""),
            "two begins": text.replace(begin_line, begin_line * 2),
            "reversed": text.replace(begin_line, "@@").replace(end_line, begin_line)
                            .replace("@@", end_line),
        }
        for label, variant in broken.items():
            with self.subTest(label), ci_project(on_push=True, ci_text=variant) as ci:
                for write_it in (False, True):
                    with self.assertRaises(cfgmod.ConfigError) as caught:
                        cfgmod.sync_ci_triggers(ci_config(True), write_it=write_it)
                    self.assertIn(cfgmod.CI_BEGIN, str(caught.exception))
                    self.assertEqual(caught.exception.where, ("ci", "on_push"))
                self.assertEqual(ci.read_text(), variant)

    def test_a_project_without_ci_yml_has_nothing_to_sync(self):
        with ci_project(on_push=False, ci_text="") as ci:
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(False), write_it=False))
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(False), write_it=True))
            self.assertFalse(ci.exists())

    def test_a_crlf_checkout(self):
        """Windows checks ci.yml out with CRLF (.gitattributes: eol=native). It
        reads as the same file, is left alone when it agrees, and is fixed when
        it does not."""
        with ci_project(on_push=True, crlf=True) as ci, quiet(), \
                contextlib.redirect_stdout(io.StringIO()):
            raw = ci.read_bytes()
            self.assertIn(b"\r\n", raw)
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(True), write_it=False))
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(True), write_it=True))
            self.assertEqual(ci.read_bytes(), raw)
            with self.assertRaises(cfgmod.ConfigError):
                cfgmod.sync_ci_triggers(ci_config(False), write_it=False)
            self.assertTrue(cfgmod.sync_ci_triggers(ci_config(False), write_it=True))
            self.assertFalse(cfgmod.sync_ci_triggers(ci_config(False), write_it=False))
            self.assertIn(cfgmod.ci_trigger_block(ci_config(False)), ci.read_text())

    def test_config_reads_another_toml_and_never_syncs(self):
        """The docs site checks the .toml blocks it shows against this
        framework with --config, and one of them may say false."""
        with tempfile.TemporaryDirectory() as tmp:
            other = Path(tmp) / "x.toml"
            other.write_text("[ci]\non_push = false\n")
            before = CI_YML.read_bytes()
            saved = cfgmod.TOML
            try:
                out, caught = run_cli("--check", "--config", str(other))
                self.assertIsNone(caught, caught)
                out, caught = run_cli("--print-ci", "--config", str(other))
                self.assertIsNone(caught, caught)
                self.assertEqual(out, "on_push=false\n")
            finally:
                cfgmod.TOML = saved
            self.assertEqual(CI_YML.read_bytes(), before)

    def test_print_ci_says_the_value(self):
        out, caught = run_cli("--print-ci")
        self.assertIsNone(caught)
        self.assertEqual(out, "on_push=true\n")

    def test_the_value_has_to_be_a_bool(self):
        for bad in ("false", "true", 0, 1, None, [False], {"on": True}):
            with self.subTest(value=bad):
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(ci_config(bad), False)
                self.assertIn("[ci] on_push", str(caught.exception))


class GradleOfflineTest(unittest.TestCase):
    """[android] gradle_offline reaches _android.yml through
    generated.properties, as the word true or false and nothing else."""

    def properties(self, cfg) -> str:
        captured = {}
        original = cfgmod.write
        cfgmod.write = lambda path, content: captured.__setitem__(str(path), content)
        try:
            cfgmod.gen_gradle_properties(cfg, ["android"])
        finally:
            cfgmod.write = original
        (text,) = captured.values()
        return text

    def test_the_default_is_offline_and_false_says_so(self):
        self.assertIs(cfgmod.DEFAULTS["android"]["gradle_offline"], True)
        self.assertRegex(toml_section("android"), r"(?m)^gradle_offline = true$")
        self.assertIn("\ngradle.offline=true\n", self.properties(base_config()))
        cfg = base_config()
        cfg["android"]["gradle_offline"] = False
        self.assertIn("\ngradle.offline=false\n", self.properties(cfg))

    def test_the_value_has_to_be_a_bool(self):
        for bad in ("false", "true", 0, 1, None, ["x"]):
            with self.subTest(value=bad):
                cfg = base_config()
                cfg["android"]["gradle_offline"] = bad
                with self.assertRaises(cfgmod.ConfigError) as caught, quiet():
                    cfgmod.validate(cfg, False)
                self.assertIn("[android] gradle_offline", str(caught.exception))


if __name__ == "__main__":
    unittest.main(verbosity=2)
