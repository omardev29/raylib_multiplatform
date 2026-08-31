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
        for good in ("clang", "gcc", "mingw", "msvc", "default"):
            with self.subTest(compiler=good), quiet():
                cfgmod.validate(base_config(dev={"compiler": good, "linker": "auto"}), False)
        self.assert_rejects(base_config(dev={"compiler": "icc", "linker": "auto"}))

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
