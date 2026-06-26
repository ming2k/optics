import os

for root, dirs, files in os.walk('tests'):
    for f in files:
        if f.endswith('.c') or f.endswith('.h') or f == 'meson.build':
            path = os.path.join(root, f)
            with open(path, 'r') as file:
                content = file.read()
            if '../../src' in content:
                content = content.replace('../../src', '../../../libs/flux/src')
                with open(path, 'w') as file:
                    file.write(content)
                print(f"Fixed {path}")
