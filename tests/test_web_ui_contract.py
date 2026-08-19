import ast
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "main" / "web_ui.h"


def test_ui_is_fail_closed_and_describes_the_final_no_jumper_topology():
    source = UI.read_text(encoding="utf-8")
    assert "Sterowanie niedostępne: wymagane są świeże ramki" in source
    assert "zworek" not in source


def test_off_state_does_not_present_placeholder_temperature_as_real():
    source = UI.read_text(encoding="utf-8")
    assert "const validSetpoint=d.state.setpoint_c>=18&&d.state.setpoint_c<=32" in source
    assert "d.state.power&&validSetpoint" in source
    assert "'— (raw '+d.state.mode_fan_code+')'" in source


def test_verified_feature_controls_and_oe_status_are_visible():
    source = UI.read_text(encoding="utf-8")
    assert "id='quiet' class='control'" in source
    assert "id='units' class='control'" in source
    assert "id='timer' class='control'" not in source
    assert "units_fahrenheit:$('#units').value==='true'" in source
    assert "Timer jest monitorowany" in source
    assert "timer:$('#timer')" not in source
    assert "id='oe'" in source


def test_utf8_polish_text_is_not_mojibake():
    source = UI.read_text(encoding="utf-8")
    assert "Łączenie" in source
    assert "Wyczyść" in source
    assert "pamięć" in source
    for broken in ("Ĺ", "Ä", "â€”", "Â"):
        assert broken not in source


def test_embedded_javascript_is_syntactically_valid():
    source = UI.read_text(encoding="utf-8")
    html = "".join(
        ast.literal_eval(line.removesuffix(";"))
        for line in source.splitlines()
        if line.startswith('"')
    )
    script = html.split("<script>", 1)[1].split("</script>", 1)[0]
    result = subprocess.run(
        ["node", "--check", "-"],
        input=script,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
