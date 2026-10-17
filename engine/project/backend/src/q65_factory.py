"""Q65 misc LittleFS layout, separate from BES1503 band factory storage."""

def local_desktop_image():
    from littlefs import LittleFS
    fs = LittleFS(block_size=8192, block_count=16, read_size=2048,
                  prog_size=2048, cache_size=2048, lookahead_size=8, mount=False)
    properties = {'persist.setup_wizard': '1',
        'persist.system_novice_guidance': 'false', 'persist.settings_novice': 'true',
        'persist.locale': 'zh_CN', 'persist.screen.auto_screen_timeout': '0',
        'persist.screen.dimmer_timeout': '2147483647'}
    fs.format()
    fs.mount()
    with fs.open('factory.prop', 'w') as stream:
        stream.write(''.join(f'{key}={value}\n' for key, value in properties.items()))
    fs.unmount()
    image = bytes(fs.context.buffer)
    fs.mount()
    try:
        with fs.open('factory.prop', 'r') as stream:
            actual = dict(line.split('=', 1) for line in stream.read().splitlines())
        if actual != properties or len(image) != 131072:
            raise ValueError('Q65 misc property remount verification failed')
    finally:
        fs.unmount()
    return image
