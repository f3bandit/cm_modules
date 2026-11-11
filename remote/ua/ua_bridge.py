#!/usr/bin/env python3
# The app link's Update All: a bridge to Update All's own settings code (its update_all.pyz on the SD card), so the app's
# Update All page shows Update All's whole list as Update All itself loads it, changes settings the way
# Update All's own menu does (its own effects and save), and uninstalls through its own database manager.
# Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md in cm_modules).
#
#   ua_bridge.py list                         every menu and entry: titles, descriptions, values, actions
#   ua_bridge.py act MENU INDEX ACTION [N]    ACTION ok or toggle on that entry; a dialog it opens comes back
#                                             as a prompt, and N answers it (the Nth button); then saved
#   ua_bridge.py preview DB_ID...             what uninstalling would remove: files and bytes, from the store
#   ua_bridge.py uninstall DB_ID...           Update All's own uninstall (the Downloader removes the files)
#   ua_bridge.py leftovers                    what Clean up offers: databases still installed but no longer turned
#                                             on in Update All (its own Downloader's list), and the Arcade
#                                             Organizer's folders (as its own Clean Folders finds them)
#   ua_bridge.py clean_ao                     Update All's own Clean Folders: the Arcade Organizer's folders go
#   ua_bridge.py serve                        the same, one JSON array of arguments per line on stdin
#
# Output: one JSON object on stdout. Update All's own prints go to stderr. Update All's start-up migrations
# (which write) are not run: `list` and `preview` write nothing.
import sys, os, json, io, contextlib, re

PYZ = os.environ.get('UA_PYZ', '/media/fat/Scripts/.config/update_all/update_all.pyz')
sys.path.insert(0, PYZ)


class Anything:
    """Stands in for Update All's curses screen: every attribute and call returns another one."""
    def __getattr__(self, name):
        return Anything()

    def __call__(self, *a, **k):
        return Anything()

    def __iter__(self):
        return iter([Anything(), Anything(), Anything()])

    def __bool__(self):
        return False


class NoScreen:
    def initialize_screen(self, *a, **k):
        return Anything(), Anything(), Anything()

    def __getattr__(self, name):
        return Anything()


class FakeUi:
    def __init__(self):
        self.values = {}
        self.effects = {}
        self.formatters = {}

    def get_value(self, key):
        return self.values.get(key, '')

    def set_value(self, key, value):
        value = value if isinstance(value, str) else str(value)
        changed = self.values.get(key) != value
        self.values[key] = value
        return changed

    def add_custom_effects(self, effects):
        self.effects.update(effects)

    def add_custom_formatters(self, formatters):
        self.formatters.update(formatters)

    def custom_effects(self):
        return self.effects

    def custom_formatters(self):
        return self.formatters


STATUS_PATHS = [os.environ.get('UA_STATUS', ''),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), 'status.json')]
ALLOW_PATH = os.environ.get('UA_ALLOW', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'allow.json'))


def read_json(paths, default):
    for p in paths:
        if p and os.path.isfile(p):
            try:
                with open(p) as f:
                    return json.load(f)
            except Exception:
                pass
    return default


class Bridge:
    def __init__(self):
        from update_all.logger import PrintLogger
        from update_all.other import GenericProvider
        from update_all.update_all_service import UpdateAllServiceFactory
        from update_all.main import read_env
        try:
            from commit import default_commit
        except ImportError:
            from update_all.constants import DEFAULT_COMMIT as default_commit
        import time
        env = read_env(default_commit, time.monotonic())
        self.service = UpdateAllServiceFactory(PrintLogger(), local_repository_provider=GenericProvider()).create(env)
        self._load_config()
        self.screen = self.service._settings_screen
        self.screen._settings_screen_printer = NoScreen()
        self.ui = FakeUi()
        self.screen.initialize_ui(self.ui)
        self.model = self.screen._model
        self.items = self.model.get('items', {})
        # every declared variable and formatter, as Update All's engine merges them for a menu
        self.variables = dict(self.model.get('variables', {}))
        self.formatters = dict(self.model.get('formatters', {}))
        for it in self.items.values():
            self.variables.update(it.get('variables', {}) or {})
            self.formatters.update(it.get('formatters', {}) or {})
        self.formatters.update(self.ui.formatters)
        self.status = read_json(STATUS_PATHS, {})
        self.allowed = set(read_json([ALLOW_PATH], []))

    # ---------- each item's status (status.json) ----------

    @staticmethod
    def key(info):
        if info.get('variable'):
            return 'var:' + info['variable']
        if info.get('uninstall') and info['uninstall'].get('db_ids'):
            return 'db:' + info['uninstall']['db_ids'][0]
        return 'entry:' + info['menu'] + '/' + info['title']

    def status_of(self, info):
        k = info['key']
        st = (self.status.get('items') or {}).get(k)
        if not st:
            for prefix, s2 in (self.status.get('menus') or {}).items():
                if info['menu'].startswith(prefix):
                    st = s2
                    break
        st = dict(st or {'status': 'untested', 'note': ''})
        vals = (self.status.get('values') or {}).get(k)
        if vals:
            st['values'] = vals
            cur = vals.get(info.get('value', ''))
            if cur:
                st['status_now'] = cur['status']
        if k in self.allowed:
            st['allowed'] = True
        return st

    def _load_config(self):
        from update_all.config import Config
        es = self.service._environment_setup
        config = Config()
        es._config_provider.initialize(config)
        es._config_reader.fill_config_with_environment(config)
        store = es._local_repository.load_store()
        es._store_provider.initialize(store)
        ini = es._config_reader.read_downloader_ini()
        es._config_reader.fill_config_with_mister_section(config, ini)
        es._config_reader.fill_config_with_local_store(config, store)
        es._config_reader.fill_config_with_database_sections(config, ini)

    # ---------- text ----------

    def interpolate(self, text):
        from update_all.ui_engine import _Interpolator
        try:
            return _Interpolator(self.formatters, self.ui).interpolate(text or '')
        except Exception:
            return re.sub(r'\{[^}]*\}', '', text or '')

    @staticmethod
    def clean(text):
        # Update All's markup: "# " titles, @highlight@, ~link~
        text = (text or '').strip()
        if text.startswith('# '):
            text = text[2:]
        return text.replace('@', '').replace('~', '')

    # ---------- the list ----------

    def entry_info(self, menu_id, idx, e):
        acts = e.get('actions') or {}
        out = {'menu': menu_id, 'index': idx, 'id': e.get('id', ''),
               'title': self.clean(self.interpolate(e.get('title', ''))),
               'desc': self.clean(self.interpolate(e.get('description', '')))}
        var = None
        for a in ('toggle', 'ok'):
            for eff in acts.get(a) or []:
                if isinstance(eff, dict) and eff.get('type') == 'rotate_variable':
                    var = var or eff.get('target')
        if var:
            out['variable'] = var
            out['value'] = self.ui.get_value(var)
            out['values'] = (self.variables.get(var) or {}).get('values', [])
        for eff in acts.get('ok') or []:
            if isinstance(eff, dict) and eff.get('type') == 'navigate' and eff.get('target') in self.items:
                out['submenu'] = eff['target']
            # a menu with an introduction first (Hybrid Cores: "About Hybrid Cores", then its menu): a menu,
            # its introduction shown with Update All's description
            elif isinstance(eff, dict) and eff.get('ui') == 'message' and len(acts.get('ok') or []) == 1:
                nav = [x.get('target') for x in eff.get('effects') or [] if isinstance(x, dict) and x.get('type') == 'navigate']
                if nav and nav[0] in self.items:
                    out['submenu'] = nav[0]
                    out['info'] = [self.clean(self.interpolate(t)) for t in eff.get('text', []) if t.strip()]
        out['actions'] = sorted(a for a in acts if a in ('ok', 'toggle', 'info'))
        info = acts.get('info')
        if info and 'info' not in out:
            for eff in info:
                if isinstance(eff, dict) and eff.get('ui') in ('message', 'confirm'):
                    out['info'] = [self.clean(self.interpolate(t)) for t in eff.get('text', [])]
        for key in ('uninstall', 'uninstall_all'):
            u = self._find_uninstall(acts.get(key))
            if u:
                out['uninstall'] = u
        out['toggles'] = 'toggle' in acts or bool(var)
        out['on'] = out.get('value') == 'true' if var else out['desc'].lower().startswith('on')
        out['key'] = self.key(out)
        out['status'] = self.status_of(out)
        return out

    def _find_uninstall(self, eff):
        # the uninstall action is a confirm whose "Yes" leads to {"ui": "uninstall_db", "db_ids": [...]}
        if isinstance(eff, dict):
            if eff.get('ui') == 'uninstall_db':
                return {'db_ids': list(eff.get('db_ids', [])), 'title': eff.get('title', ''),
                        'on_success': eff.get('on_success')}
            for v in eff.values():
                r = self._find_uninstall(v)
                if r:
                    return r
        elif isinstance(eff, list):
            for v in eff:
                r = self._find_uninstall(v)
                if r:
                    return r
        return None

    def _update_all_only(self, e):
        # what only makes sense inside Update All: its SAVE / EXIT, the account and patron menus
        flat = json.dumps(e.get('actions') or {})
        if any(k in flat for k in ('calculate_needs_save', 'device_login', 'calculate_has_right_available_code',
                                   '"target": "abort"', 'patrons_menu')):
            return True
        # the account's own menu (the app has its own Account screen); a setting that only reads the account's
        # benefits (Coin-Op's Releases, JTCORES' Private Releases: Auto follows the benefit) stays
        if re.search(r'"target": "retroaccount_[a-z_]*menu"', flat):
            return True
        t = self.clean(self.interpolate(e.get('title', ''))).strip().upper()
        return t.startswith(('SAVE', 'EXIT')) or t in ('BACK', '#', '')

    def list(self):
        menus = {}
        for mid, it in self.items.items():
            if mid.startswith('test_') or mid.startswith('retroaccount_'):
                continue
            entries = []
            for i, e in enumerate(it.get('entries', []) or []):
                if not e or not e.get('title') or self._update_all_only(e):
                    continue
                entries.append(self.entry_info(mid, i, e))
            menus[mid] = {'title': self.clean(self.interpolate(it.get('header', ''))), 'entries': entries}
        # a menu entry with no word of its own: what's inside it, counted (its menus' items, all the way down)
        def inside(mid, seen):
            n = {}
            if mid in seen or mid not in menus:
                return n
            seen.add(mid)
            for e in menus[mid]['entries']:
                if e.get('submenu') and not e.get('toggles') and e['status'].get('status') == 'menu':
                    for k, v in inside(e['submenu'], seen).items():
                        n[k] = n.get(k, 0) + v
                else:
                    st = e['status'].get('status_now') or e['status'].get('status')
                    n[st] = n.get(st, 0) + 1
            return n
        for m in menus.values():
            for e in m['entries']:
                if e.get('submenu') and not e.get('toggles') and e['status'].get('status') == 'untested' \
                        and e['key'] not in (self.status.get('items') or {}):
                    e['status'] = {'status': 'menu', 'note': ''}
        for m in menus.values():
            for e in m['entries']:
                if e['status'].get('status') == 'menu':
                    n = inside(e['submenu'], set())
                    words = [('merged', 'built in'), ('safe', 'safe'), ('guard', 'guarded'), ('warn', 'with a warning'),
                             ('keep', 'left alone'), ('untested', 'untested')]
                    parts = ['%d %s' % (n[k], w) for k, w in words if n.get(k)]
                    e['status']['note'] = ('Inside: ' + ', '.join(parts) + '.') if parts else ''
                    e['status']['counts'] = n
        root = 'main_menu_account' if 'main_menu_account' in menus else 'main_menu_login'
        installed = sorted(k[:-len('_installed')] for k, v in self.ui.values.items() if k.endswith('_installed') and v == 'true')
        return {'ok': True, 'version': self.ui.get_value('update_all_version'), 'root': root, 'menus': menus,
                'installed': installed}

    # ---------- changing things ----------

    def act(self, menu_id, idx, action, choice=None):
        from update_all.ui_engine import _EffectResolver
        it = self.items[menu_id]
        e = (it.get('entries') or [])[idx]
        chain = (e.get('actions') or {}).get(action) or []
        data = {'variables': self.variables, 'formatters': self.formatters}
        # a setting that follows the account (Coin-Op's Releases on Auto: the benefit's level): the account's
        # benefits read first, once, as Update All's own menu has them when it opens
        if 'retroaccount_' in json.dumps(chain) and not getattr(self, '_account_read', False):
            self._account_read = True
            try:
                self.account()
            except Exception:
                pass
        # the guard: a guarded item isn't turned on (nor a guarded value chosen) unless allowed
        before = self.entry_info(menu_id, idx, e)
        if action in ('ok', 'toggle') and not before['status'].get('allowed'):
            if before['status']['status'] == 'guard' and not before['on'] and not before.get('submenu'):
                return {'ok': False, 'guarded': True, 'entry': before,
                        'error': before['status'].get('note') or 'guarded'}
            vals = before['status'].get('values')
            if vals and before.get('variable'):
                values = before.get('values') or []
                if before['value'] in values:
                    nxt = values[(values.index(before['value']) + 1) % len(values)]
                    if (vals.get(nxt) or {}).get('status') == 'guard':
                        # skip the guarded values: straight to the next one that isn't
                        n = values.index(nxt)
                        for _ in values:
                            n = (n + 1) % len(values)
                            if (vals.get(values[n]) or {}).get('status') != 'guard':
                                break
                        self.ui.set_value(before['variable'], values[n])
                        self.save()
                        return {'ok': True, 'saved': True, 'skipped': nxt, 'entry': self.entry_info(menu_id, idx, e)}
        # the chain runs up to its first dialog: what it changed before a confirm is undone until the confirm is
        # answered (the answer replays the chain from the real values, then the chosen button's effects)
        before_values = dict(self.ui.values)
        before_edits = dict(getattr(self.screen, '_pending_mister_ini_edits', {}) or {})
        res = _EffectResolver(self.ui, data, self.ui.effects).resolve_effect_chain(chain)
        nav = self._navigate_of(res)
        if isinstance(res, dict) and res.get('ui') == 'confirm' and choice is None:
            self.ui.values = before_values
            if hasattr(self.screen, '_pending_mister_ini_edits'):
                self.screen._pending_mister_ini_edits = before_edits
        if isinstance(res, dict) and res.get('ui') in ('message', 'confirm'):
            buttons = [a.get('title', '') for a in res.get('actions', []) if isinstance(a, dict)]
            if choice is None or res.get('ui') == 'message':
                prompt = {'ui': res.get('ui'), 'header': self.clean(self.interpolate(res.get('header', ''))),
                          'text': [self.clean(self.interpolate(t)) for t in res.get('text', [])], 'buttons': buttons}
                if res.get('ui') == 'confirm':
                    return {'ok': True, 'prompt': prompt, 'saved': False}
                self.save()                          # (a message only reports: the change stands)
                r = {'ok': True, 'prompt': prompt, 'saved': True}
                if nav:
                    r['navigate'] = nav
                return r
            picked = [a for a in res.get('actions', []) if isinstance(a, dict)][int(choice)]
            if picked.get('type') == 'fixed':
                nav = self._navigate_of(_EffectResolver(self.ui, data, self.ui.effects).resolve_effect_chain(picked.get('fixed', [])))
        self.save()
        r = {'ok': True, 'saved': True, 'entry': self.entry_info(menu_id, idx, e)}
        if nav:
            r['navigate'] = nav
        return r

    def _navigate_of(self, res):
        # Update All's engine hands a navigate back to its screen: the menu to open, when it's one of the list's
        if isinstance(res, str) and res in self.items:
            return res
        if isinstance(res, dict):
            if res.get('type') == 'navigate' and res.get('target') in self.items:
                return res['target']
            for x in res.get('effects') or []:
                if isinstance(x, dict) and x.get('type') == 'navigate' and x.get('target') in self.items:
                    return x['target']
        return None

    def save(self):
        self.screen.save(self.ui)

    # ---------- uninstall ----------

    def preview(self, db_ids):
        store = self.service._environment_setup._local_repository
        try:
            path = self.service._ini_repository.downloader_store_standard_path() if hasattr(self.service, '_ini_repository') else None
        except Exception:
            path = None
        path = path or '/media/fat/Scripts/.config/downloader/downloader.json'
        with open(path) as f:
            dbs = json.load(f).get('dbs', {})
        files = size = 0
        sample = []
        for db in db_ids:
            d = dbs.get(db) or dbs.get(db.lower()) or {}
            for p, desc in (d.get('files') or {}).items():
                files += 1
                size += int((desc or {}).get('size', 0) or 0)
                if len(sample) < 8:
                    sample.append(p)
        return {'ok': True, 'files': files, 'bytes': size, 'sample': sample}

    # ---------- clean up ----------

    def _store_dbs(self):
        path = '/media/fat/Scripts/.config/downloader/downloader.json'
        try:
            with open(path) as f:
                return {k.lower(): v for k, v in (json.load(f).get('dbs') or {}).items()}
        except Exception:
            return {}

    @staticmethod
    def _folder_size(top):
        files = size = 0
        for root, dirs, names in os.walk(top):          # (links aren't followed: the organizer's are links)
            for n in names:
                files += 1
                try:
                    st = os.lstat(os.path.join(root, n))
                    size += st.st_size if not os.path.islink(os.path.join(root, n)) else 0
                except OSError:
                    pass
        return files, size

    def leftovers(self):
        # databases the Downloader still has installed that Update All no longer has turned on: their files
        # stay on the SD card until they're uninstalled (Update All's own Downloader --list-dbs)
        dbs = []
        installed = self.screen._database_manager_service.list_installed_dbs() or []
        store = self._store_dbs()
        for d in installed:
            if d.configured:
                continue
            files = size = 0
            sample = []
            for p, desc in ((store.get(d.db_id.lower()) or {}).get('files') or {}).items():
                path = os.path.join('/media/fat', p.lstrip('|'))
                if not os.path.lexists(path):
                    continue                              # (already gone)
                files += 1
                size += int((desc or {}).get('size', 0) or 0)
                if len(sample) < 8:
                    sample.append(p.lstrip('|'))
            dbs.append({'db_id': d.db_id, 'title': d.title or d.db_id, 'files': files, 'bytes': size, 'sample': sample})
        # the Arcade Organizer's folders, as its own Clean Folders finds them
        ao = {'folders': [], 'files': 0, 'bytes': 0, 'on': self.ui.get_value('arcade_organizer') == 'true'}
        try:
            self.screen.calculate_arcade_organizer_folders(self.ui)
            for line in self.ui.get_value('arcade_organizer_folders_list').splitlines():
                line = line.strip()
                if line and os.path.isdir(line):
                    f, b = self._folder_size(line)
                    ao['folders'].append(line)
                    ao['files'] += f
                    ao['bytes'] += b
        except Exception as e:
            ao['error'] = str(e)
        return {'ok': True, 'dbs': dbs, 'arcade_organizer': ao}

    def clean_ao(self):
        self.screen.calculate_arcade_organizer_folders(self.ui)
        folders = [l.strip() for l in self.ui.get_value('arcade_organizer_folders_list').splitlines() if l.strip()]
        self.screen.clean_arcade_organizer_folders(self.ui)
        return {'ok': True, 'removed': folders}

    def _uninstall_effect(self, db_ids):
        want = sorted(x.lower() for x in db_ids)
        for it in self.items.values():
            for e in it.get('entries') or []:
                for key in ('uninstall', 'uninstall_all'):
                    u = self._find_uninstall(((e or {}).get('actions') or {}).get(key))
                    if u and sorted(x.lower() for x in u['db_ids']) == want:
                        return u
        return None

    def dbs(self):
        # Update All's Database Manager (System Options): every database the Downloader has installed, turned
        # on or not, with what it put on the SD card (its own --list-dbs, as Update All's screen lists them)
        installed = self.screen._database_manager_service.list_installed_dbs()
        if installed is None:
            return {'ok': False, 'error': "Update All's Downloader couldn't list the installed databases"}
        store = self._store_dbs()
        out = []
        for d in installed:
            files = size = 0
            for p, desc in ((store.get(d.db_id.lower()) or {}).get('files') or {}).items():
                files += 1
                size += int((desc or {}).get('size', 0) or 0)
            out.append({'db_id': d.db_id, 'title': d.title or d.db_id, 'description': d.description or '',
                        'configured': bool(d.configured), 'url': d.db_url or '', 'files': files, 'bytes': size})
        return {'ok': True, 'dbs': out}

    def update_db(self, db_id):
        # Update Only: that one database fetched by Update All's Downloader (--run-only), nothing else
        code = self.screen._database_manager_service.update(db_id)
        return {'ok': code == 0, 'code': code}

    def install(self, db_id):
        # turned on the way Update All's own menu does (its variable for that database), saved, then that one
        # database fetched by Update All's Downloader (--run-only), as Update All's database manager updates one
        lid = db_id.lower()
        var = None
        for v in self.variables:
            if v.lower() == lid:
                var = v
        st = self.status_of({'key': ('var:' + var) if var else ('db:' + db_id), 'menu': '', 'value': 'true'})
        if st.get('status') == 'guard' and not st.get('allowed'):
            return {'ok': False, 'guarded': True, 'error': st.get('note') or 'guarded'}
        if var:
            self.ui.set_value(var, 'true')
        self.save()
        service = self.screen._database_manager_service
        code = service.update(db_id)
        return {'ok': code == 0, 'code': code}

    def uninstall(self, db_ids):
        from update_all.ui_engine import _EffectResolver
        service = self.screen._database_manager_service
        code = service.uninstall(list(db_ids))
        if code != 0:
            return {'ok': False, 'code': code}
        # Update All's own menu then turns it off (its variable, <id>_installed, MiSTer.ini lines, artwork
        # and manuals selections): the same chain, then saved, so the next run doesn't put it back
        u = self._uninstall_effect(db_ids)
        chain = [x for x in ((u or {}).get('on_success') or []) if not (isinstance(x, dict) and x.get('type') == 'navigate')]
        if chain:
            data = {'variables': self.variables, 'formatters': self.formatters}
            _EffectResolver(self.ui, data, self.ui.effects).resolve_effect_chain(chain)
        self.save()
        return {'ok': True, 'code': 0}


    # ---------- Update All's account (RetroAccount): the Patreon supporters' extras, JTCORES premium (jtbeta),
    # the Coin-Op collection; signed in with a code on a phone or PC, the way Update All's own menu does ----------

    def _ra(self):
        return self.screen._retroaccount

    def account(self):
        ra = self._ra()
        out = {'ok': True, 'logged_in': bool(ra.get_login_state())}
        if out['logged_in']:
            try:
                # what the account gives (and its files: the Patreon key, JOTEGO's jtbeta, the Coin-Op license),
                # synced the way Update All's own menu and runs do
                from update_all.update_output import NoopUpdateOutput
                ra.mister_sync(NoopUpdateOutput())
            except Exception as e:
                out['check_error'] = str(e)
            try:
                self.screen.retroaccount_check_state(self.ui)
            except Exception as e:                       # (the server can be unreachable: what's known is shown)
                out['check_error'] = str(e)
            for k in ('update_all_extras', 'update_all_extras_support', 'jtbeta_access', 'jtbeta_access_support',
                      'coin_op_access', 'coin_op_access_support', 'device_verified', 'device_verification_description',
                      'device_verification_message', 'verified_chip_id_message', 'domain'):
                v = self.ui.get_value('retroaccount_' + k)
                out[k] = self.clean(self.interpolate(str(v if v is not None else '')))
        return out

    def login_start(self):
        ra = self._ra()
        r = ra.request_device_code()
        if not r:
            return {'ok': False, 'error': "Couldn't reach RetroAccount: check the SuperStation's internet connection"}
        code = r.get('user_code', '')
        base = str(getattr(ra, 'server_url', 'https://retroaccount.com')).rstrip('/')
        return {'ok': True, 'user_code': code, 'verification_uri': r.get('verification_uri') or f'{base}/code?c={code}',
                'device_code': r.get('device_code', ''), 'device_id': r.get('device_id') or ra.get_existing_device_id() or '',
                'expires_in': int(r.get('expires_in') or 600), 'interval': int(r.get('interval') or 5)}

    def login_poll(self, device_code, device_id):
        ra = self._ra()
        t = ra.poll_for_token(device_code, device_id or None)
        if not t:
            return {'ok': True, 'state': 'waiting'}
        if t.get('status') == 'unauthorized':
            return {'ok': True, 'state': 'denied'}
        cred = t.get('credentials', t)
        if isinstance(cred, dict) and 'device_id' not in cred:
            cred['device_id'] = t.get('device_id', device_code)
        if not ra.save_login_credentials(cred):
            return {'ok': False, 'error': "Couldn't store the login on the SD card"}
        return {'ok': True, 'state': 'signed_in'}

    def logout(self):
        self.screen.retroaccount_device_logout(self.ui)
        return {'ok': True, 'logged_in': bool(self._ra().get_login_state())}

    def link_fpga(self):
        msg = self.screen.retroaccount_attach_chip_id_to_device(self.ui)
        out = self.account()
        out['result'] = msg if isinstance(msg, str) else ''
        return out


def run(b, args):
    cmd = args[0]
    if cmd == 'list':
        return b.list()
    if cmd == 'act':
        return b.act(args[1], int(args[2]), args[3], int(args[4]) if len(args) > 4 and args[4] is not None else None)
    if cmd == 'allow':                        # allow KEY on|off: the guard lets that item through
        allowed = set(read_json([ALLOW_PATH], []))
        (allowed.add if args[2] == 'on' else allowed.discard)(args[1])
        os.makedirs(os.path.dirname(ALLOW_PATH), exist_ok=True)
        with open(ALLOW_PATH + '.tmp', 'w') as f:
            json.dump(sorted(allowed), f)
        os.replace(ALLOW_PATH + '.tmp', ALLOW_PATH)
        b.allowed = allowed
        return {'ok': True, 'allowed': sorted(allowed)}
    if cmd == 'preview':
        return b.preview(args[1:])
    if cmd == 'uninstall':
        return b.uninstall(args[1:])
    if cmd == 'install':
        return b.install(args[1])
    if cmd == 'leftovers':
        return b.leftovers()
    if cmd == 'dbs':
        return b.dbs()
    if cmd == 'update_db':
        return b.update_db(args[1])
    if cmd == 'clean_ao':
        return b.clean_ao()
    if cmd == 'account':
        return b.account()
    if cmd == 'login_start':
        return b.login_start()
    if cmd == 'login_poll':
        return b.login_poll(args[1], args[2] if len(args) > 2 else '')
    if cmd == 'logout':
        return b.logout()
    if cmd == 'link_fpga':
        return b.link_fpga()
    return {'ok': False, 'error': 'unknown command'}


def serve():
    # kept running by cmremote while the page is open: Update All's code loads once (seconds on the SuperStation);
    # one JSON array of arguments per line in, one JSON object per line out
    real_out = sys.stdout
    with contextlib.redirect_stdout(sys.stderr):
        b = Bridge()
    real_out.write(json.dumps({'ok': True, 'ready': True}) + '\n')
    real_out.flush()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            args = json.loads(line)
            with contextlib.redirect_stdout(sys.stderr):
                r = run(b, args)
                if args and args[0] in ('uninstall', 'install', 'update_db'):
                    try:
                        b = Bridge()                  # what's installed changed: read it all again
                    except Exception:
                        pass                          # (cmremote reloads it on the next list)
        except Exception as e:
            import traceback
            r = {'ok': False, 'error': str(e), 'trace': traceback.format_exc()[-1500:]}
        real_out.write(json.dumps(r) + '\n')
        real_out.flush()
    return 0


def main():
    args = sys.argv[1:] or ['list']
    if args[0] == 'serve':
        return serve()
    real_out = sys.stdout
    with contextlib.redirect_stdout(sys.stderr):    # Update All's own prints
        r = run(Bridge(), args)
    json.dump(r, real_out)
    return 0 if r.get('ok') else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:
        import traceback
        json.dump({'ok': False, 'error': str(e), 'trace': traceback.format_exc()[-2000:]}, sys.stdout)
        sys.exit(1)
