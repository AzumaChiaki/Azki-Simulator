"""Fixed q65 physical clicks through the authenticated instance runner."""
import time

from .adc_commands import DEADLINE_SECONDS, runner_exchange
from .minative import MError, _decode


def supports_buttons(plan):
    if type(plan) is not dict or type(plan.get('identity')) is not dict:
        return False
    profile = plan['identity'].get('profile')
    return type(profile) is dict and profile.get('model') == 'miwear.watch.q65'


def click_button(adapter, arguments):
    if (type(arguments) is not dict or set(arguments) != {'button', 'clicks'} or
            type(arguments['button']) is not str or arguments['button'] not in ('lower', 'crown') or
            type(arguments['clicks']) is not int or arguments['clicks'] not in (1, 2) or
            (arguments['button'] == 'crown' and arguments['clicks'] != 1)):
        raise MError('invalid_button', '下键支持单击/双击，表冠仅支持单击')
    if adapter is None:
        raise MError('not_running', '请先启动红米手表7')
    if not supports_buttons(adapter.plan):
        raise MError('unsupported_hardware', '此按钮仅支持 miwear.watch.q65')
    payload = runner_exchange(adapter, {'command': 'q65.button', **arguments}, write=True,
                              deadline=time.monotonic() + DEADLINE_SECONDS)
    result = _decode(payload)
    if type(result) is dict and set(result) == {'error'}:
        raise MError('button_rejected', str(result['error'])[:300] + '；不要自动重试')
    expected = {'accepted': True, 'guestObserved': False, **arguments}
    value = result.get('return') if type(result) is dict and set(result) == {'return'} else None
    if (type(value) is not dict or set(value) != set(expected) or
            any(type(value[k]) is not type(v) or value[k] != v for k, v in expected.items())):
        raise MError('button_outcome_unknown', '按键可能已生效但确认无效，不要自动重试')
    return value
