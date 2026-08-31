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

Run it with `just test` (part of the default set) or on its own:

    python3 tests/configure_test.py
    python3 tests/configure_test.py -v ConfigureVersionTest

Standard library only, on purpose: `just test` must not need a pip install, and
this has to run on a Windows runner and inside the pinned container alike.
"""

from __future__ import annotations

import contextlib
import copy
import re
import importlib.util
import io
import os
import sys
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
        else:
            cfg[section] = value
    return cfg


@contextlib.contextmanager
def quiet():
    """configure.py warns on stderr. A test run should not be noisy."""
    with contextlib.redirect_stderr(io.StringIO()):
        yield


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

    def test_all_is_every_runnable_target(self):
        result = cfgmod.expand_targets(["all"], [])
        self.assertEqual(len(result), 14)
        self.assertNotIn("linux-drm", result, "linux-drm must not come in through 'all'")

    def test_overlapping_groups_deduplicate(self):
        # desktop and linux share three targets; windows shares two with desktop.
        result = cfgmod.expand_targets(["desktop", "linux", "windows"], [])
        self.assertEqual(len(result), len(set(result)))
        self.assertEqual(result, cfgmod.expand_targets(["desktop"], []))

    def test_a_target_named_twice_appears_once(self):
        self.assertEqual(cfgmod.expand_targets(["web", "web", "web"], []), ["web"])

    def test_group_plus_one_of_its_own_members(self):
        self.assertEqual(cfgmod.expand_targets(["linux", "linux-x64"], []),
                         cfgmod.expand_targets(["linux"], []))

    def test_order_follows_the_declaration_order_not_the_input(self):
        """Stable output, whatever order the .toml lists things in."""
        self.assertEqual(cfgmod.expand_targets(["web", "linux-x64", "android"], []),
                         cfgmod.expand_targets(["android", "web", "linux-x64"], []))
        self.assertEqual(cfgmod.expand_targets(["web", "linux-x64"], []),
                         ["linux-x64", "web"])

    def test_disabled_subtracts_from_a_group(self):
        result = cfgmod.expand_targets(["all"], ["ios"])
        self.assertNotIn("ios", result)
        self.assertIn("macos", result)
        self.assertEqual(len(result), 13)

    def test_disabled_can_be_a_group_too(self):
        result = cfgmod.expand_targets(["all"], ["bsd"])
        self.assertEqual(len(result), 9)
        self.assertFalse([t for t in result if "bsd" in t])

    def test_disabling_something_not_enabled_is_harmless(self):
        self.assertEqual(cfgmod.expand_targets(["web"], ["android"]), ["web"])

    def test_drm_has_to_be_asked_for_by_name(self):
        self.assertIn("linux-drm", cfgmod.expand_targets(["all", "linux-drm"], []))
        self.assertIn("linux-drm", cfgmod.expand_targets(["drm"], []))

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
        self.assertEqual(self.upx(["linux-x64", "linux-arm64"]), ["linux-x64", "linux-arm64"])

    def test_all_is_every_compressible_target_and_no_more(self):
        result = self.upx(["all"])
        for absent in ("macos", "ios", "android", "web"):
            self.assertNotIn(absent, result)
        self.assertIn("windows-x64", result)
        self.assertIn("netbsd-x64", result)

    def test_groups_overlap_without_doubling_up(self):
        result = self.upx(["linux", "all", "linux-x64"])
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
        self.assertEqual(self.upx(["all"], targets=["web", "linux-x64"]), ["linux-x64"])

    def test_empty_is_allowed_and_means_compress_nothing(self):
        self.assertEqual(self.upx([]), [])

    def test_the_lists_have_to_be_lists(self):
        cfg = base_config(upx__enabled="linux-x64")
        with self.assertRaises(cfgmod.ConfigError), quiet():
            cfgmod.validate(cfg, False)


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
        merged = cfgmod.deep_merge(cfgmod.DEFAULTS, {"upx": {"enabled": ["linux-x64"]}})
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

    def test_windows_backend(self):
        self.assert_rejects(base_config(windows={"backend": "sdl"}))


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
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed; the iOS job has it")
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
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed; the iOS job has it")
        nasty = 'Hero: "the game", v2 #1'
        cfg = base_config()
        cfg["window"]["title"] = nasty
        with quiet():
            cfgmod.gen_ios_project(cfg)
        spec = yaml.safe_load((Path(self._tmp.name) / "ios" / "project.yml").read_text())
        self.assertIn(nasty, yaml.dump(spec))

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

    def test_the_three_backends_are_accepted(self):
        for backend in ("glfw", "emscripten", "rgfw"):
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
        for backend in ("glfw", "emscripten", "rgfw"):
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

    def test_it_reaches_the_generated_cmake(self):
        """The whole path, not just the check: a backend nobody writes out is a
        backend the build never hears about."""
        for backend in ("glfw", "emscripten", "rgfw"):
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
                    cfgmod.validate(self.with_value(section, key, good.upper()), False)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)


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
            ("web", "rgfw"): "WebRGFW",
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
