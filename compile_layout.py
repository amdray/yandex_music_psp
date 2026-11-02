"""Compile the editor's layout into named, immutable C objects."""
import argparse
from pathlib import Path
from layout_model import load_layout


def generate(screens):
    lines = ['/* Generated from assets/ui_layout.txt; do not edit. */',
             '#ifndef YM_UI_LAYOUT_GENERATED_H', '#define YM_UI_LAYOUT_GENERATED_H']
    for screen in screens:
        lines.append(f'extern const UiLayoutScreen ui_layout_screen_{screen.name};')
        for widget in screen.widgets:
            lines.append(f'extern const UiLayoutWidget {widget.symbol};')
    lines.append('#ifdef UI_LAYOUT_IMPLEMENTATION')
    for screen in screens:
        for w in screen.widgets:
            geometry = ', '.join(f'{v!r}f' for v in (w.x, w.y, w.width, w.height))
            lines.append(f'const UiLayoutWidget {w.symbol} = {{UI_LAYOUT_WIDGET_{w.kind.upper()}, '
                         f'{geometry}, 0x{w.color:08X}U, 0x{w.secondary_color:08X}U, '
                         f'0x{w.background_color:08X}U, {w.step!r}f}};')
        if screen.widgets:
            lines.append(f'static const UiLayoutWidget *const widgets_{screen.name}[] = {{')
            lines.extend(f'    &{w.symbol},' for w in screen.widgets)
            lines.append('};')
        array = f'widgets_{screen.name}' if screen.widgets else 'NULL'
        lines.append(f'const UiLayoutScreen ui_layout_screen_{screen.name} = '
                     f'{{0x{screen.background:08X}U, {len(screen.widgets)}, {array}}};')
    lines.extend(('#endif', '#endif', ''))
    return '\n'.join(lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        result = generate(load_layout(args.source))
    except ValueError as exc:
        parser.exit(1, f'{exc}\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix('.tmp')
    temporary.write_text(result, encoding='utf-8', newline='\n')
    temporary.replace(args.output)
