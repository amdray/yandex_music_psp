"""Layout source shared by the editor and the build-time C generator."""
from dataclasses import dataclass, field
import math
from pathlib import Path
import re
import struct


def identifier(value):
    if not re.fullmatch(r"[a-z][a-z0-9_]*", value):
        raise ValueError(f"invalid identifier: {value!r}")
    return value


def color(value):
    value = value.removeprefix('#').removeprefix('0x').removeprefix('0X')
    if not re.fullmatch(r'[0-9a-fA-F]{8}', value):
        raise ValueError(f"expected eight ABGR hex digits: {value!r}")
    return int(value, 16)


def scalar(value):
    result = float(value)
    if not math.isfinite(result) or not math.isfinite(struct.unpack('f', struct.pack('f', result))[0]):
        raise ValueError(f"not a finite C float: {value!r}")
    return struct.unpack('f', struct.pack('f', result))[0]


def number(value):
    return str(int(value)) if value == int(value) else repr(value)


@dataclass
class Widget:
    widget_id: str
    kind: str
    x: float
    y: float
    width: float
    height: float
    color: int
    screen_name: str
    step: float = 0.0
    secondary_color: int = 0
    background_color: int = 0

    @property
    def binding(self):
        # Editor dispatch identity is derived, never independently configured.
        return f'{self.screen_name}.{self.widget_id}'

    @property
    def symbol(self):
        role = 'rows' if self.kind == 'slot' and self.step > 0 else self.kind
        return f'ui_layout_{role}_{self.screen_name}__{self.widget_id}'

    @classmethod
    def parse(cls, value, screen_name):
        f = [part.strip() for part in value.split('|')]
        if not 7 <= len(f) <= 10:
            raise ValueError('widget needs 7 to 10 fields')
        identifier(f[0])
        if f[1] not in ('rect', 'slot'):
            raise ValueError(f'unknown widget type: {f[1]!r}')
        geometry = [scalar(v) for v in f[2:6]]
        if geometry[2] < 0 or geometry[3] < 0:
            raise ValueError('negative widget size')
        step = scalar(f[7]) if len(f) > 7 and f[7] else 0.0
        if step < 0:
            raise ValueError('negative step')
        return cls(f[0], f[1], *geometry, color(f[6]), screen_name, step,
                   color(f[8]) if len(f) > 8 and f[8] else 0,
                   color(f[9]) if len(f) > 9 and f[9] else 0)

    def encode(self):
        f = [self.widget_id, self.kind, *map(number, (self.x, self.y, self.width, self.height)),
             f'{self.color:08X}', number(self.step) if self.step else '']
        if self.secondary_color or self.background_color:
            f.extend((f'{self.secondary_color:08X}', f'{self.background_color:08X}'))
        return 'widget=' + '|'.join(f)


@dataclass
class Screen:
    name: str
    background: int = 0xFF1A1A1A
    widgets: list[Widget] = field(default_factory=list)


def load_layout(path):
    screens = []
    current = None
    for line_number, raw in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(('#', ';')):
            continue
        try:
            if line.startswith('[') and line.endswith(']'):
                name = identifier(line[1:-1].strip())
                if any(s.name == name for s in screens):
                    raise ValueError('duplicate screen')
                current = Screen(name)
                screens.append(current)
                continue
            if current is None or '=' not in line:
                raise ValueError('property outside a screen')
            key, value = map(str.strip, line.split('=', 1))
            if key == 'background':
                current.background = color(value)
            elif key == 'widget':
                widget = Widget.parse(value, current.name)
                if any(w.widget_id == widget.widget_id for w in current.widgets):
                    raise ValueError('duplicate widget')
                current.widgets.append(widget)
            else:
                raise ValueError(f'unknown property: {key!r}')
        except (ValueError, OverflowError) as exc:
            raise ValueError(f'{path}:{line_number}: {exc}') from exc
    if not screens:
        raise ValueError(f'{path}: no screens')
    return screens


def save_layout(path, screens):
    lines = ['# YMPSP UI layout v2',
             '# widget=id|type|x|y|width|height|ABGR|step|secondary_ABGR|background_ABGR', '']
    for screen in screens:
        lines.extend((f'[{screen.name}]', f'background={screen.background:08X}'))
        lines.extend(w.encode() for w in screen.widgets)
        lines.append('')
    Path(path).write_text('\n'.join(lines), encoding='utf-8', newline='\n')
