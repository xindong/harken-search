"""Real inline metadata editing checks, using only app-local input and synthetic files."""
from pathlib import Path
import sqlite3
import time
import uuid
import wave


def run_edit_suite(args, command, settled, click, capture, process, checks):
    root=args.isolation_root.resolve()/('edit-fixture-'+args.output.name)
    root.mkdir(parents=True,exist_ok=True)
    audio=root/'audio'; audio.mkdir(exist_ok=True)
    databases=[root/'library'/ 'main.db',root/'other'/ 'main.db']
    for db in databases:
        db.parent.mkdir(exist_ok=True)
        if not db.exists():
            with sqlite3.connect(db) as connection: connection.execute('PRAGMA user_version=1')
    for name in ['first.wav','second.wav']:
        with wave.open(str(audio/name),'wb') as output:
            output.setparams((1,2,22050,0,'NONE','not compressed'))
            output.writeframes(b'\x00\x00'*22050)

    def wait_for(probe, predicate, timeout=40):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            value=probe()
            if predicate(value): return value
            time.sleep(.1)
        raise AssertionError(value)

    def select(row):
        command(cmd='select_row',row=row)
        settled()

    def double_click():
        command(cmd='info_page_pointer',key='info.metadataContent',phase='hover')
        for phase in ['down','up','down','up']:
            command(cmd='info_page_pointer',key='info.metadataContent',phase=phase)
            time.sleep(.015)
        current=settled()
        assert current['editing'],current
        return current

    def change(text):
        click('info.edit.description')
        command(cmd='info_edit_input',text=text)
        current=settled()
        assert text in current['draft_description'],current
        return current

    command(cmd='switch_db',path=str(databases[0]))
    wait_for(lambda:command(cmd='get_state'),lambda s:not s['db_switch_in_progress'] and bool(s['db_path']))
    command(cmd='scan_folder',path=str(audio),cache_waveforms=False,build_similarity_index=False)
    wait_for(lambda:command(cmd='scan_status'),lambda s:not s['scanning'])
    command(cmd='scan_acknowledge_complete')
    if 'info.qaScanCancel' in settled()['zones']: click('info.qaScanCancel')
    command(cmd='search',text='')
    wait_for(lambda:command(cmd='get_state'),lambda s:s['result_count']>=2)
    command(cmd='info_page_layout',floating=False,width=520); settled()
    command(cmd='info_page_layout',floating=True,width=520); settled()
    select(0); click('info.metadataSummary')
    capture('metadata-no-pencil')
    double_click(); click('info.artwork')
    assert not settled()['awaiting_save'],'unchanged editing must leave directly'
    click('info.metadataSummary'); double_click()
    first=change('saved-first')
    capture('inline-edit-no-buttons')
    click('info.artwork')
    pending=settled(); assert pending['awaiting_save'] and pending['selected']==0,pending
    capture('save-required','info.saveYes')
    command(cmd='info_edit_input',action='escape_down'); settled()
    command(cmd='info_edit_input',action='escape_up')
    assert settled()['awaiting_save'],'Escape must not dismiss the choice'
    current=click('info.saveYes')
    assert not current['editing'] and current['selected']==1,current
    assert current['saved_description']==first['draft_description'],current
    checks.append({'case':'page-save-and-mandatory-choice','state':current})

    click('info.metadataSummary'); double_click(); baseline=settled()['saved_description']
    change('discard-this'); command(cmd='select_row',row=1)
    pending=settled(); assert pending['awaiting_save'] and pending['edit_id']==first['edit_id'],pending
    current=click('info.saveNo')
    assert current['saved_description']==baseline and not current['editing'],current
    checks.append({'case':'file-switch-discard-original-record','state':current})

    double_click(); change('saved-second'); click('info.close')
    assert settled()['awaiting_save'] and settled()['visible']
    current=click('info.saveYes'); assert not current['visible'] and not current['editing'],current
    checks.append({'case':'panel-close-save','state':current})
    command(cmd='info_page_layout',floating=True,width=520); settled()
    select(0); double_click(); change('save-must-fail')
    db_path=Path(command(cmd='get_state')['db_path'])
    assert db_path.resolve().is_relative_to(root)
    lock=sqlite3.connect(db_path)
    lock.execute('BEGIN IMMEDIATE')
    try:
        click('info.artwork'); current=click('info.saveYes')
        assert current['awaiting_save'] and current['editing'] and current['save_error'],current
        capture('save-failure-keeps-draft','info.saveYes')
        checks.append({'case':'save-failure-keeps-draft','state':current})
        click('info.saveNo')
    finally:
        lock.rollback(); lock.close()

    click('info.metadataSummary'); select(0); double_click(); change('before-db-switch')
    old_path=command(cmd='get_state')['db_path']
    try: command(cmd='switch_db',path=str(databases[1]))
    except RuntimeError:
        if not settled()['awaiting_save']: raise
    assert settled()['awaiting_save']
    assert command(cmd='get_state')['db_path']==old_path
    current=click('info.saveYes')
    wait_for(lambda:command(cmd='get_state'),lambda s:not s['db_switch_in_progress'] and s['db_path']!=old_path)
    if 'info.qaScanCancel' in settled()['zones']: click('info.qaScanCancel')
    checks.append({'case':'database-switch-save-before-switch','state':current})
    command(cmd='switch_db',path=str(databases[0]))
    wait_for(lambda:command(cmd='get_state'),lambda s:not s['db_switch_in_progress'] and s['result_count']>=2)
    if args.edit_new_db_suite:
        select(0); double_click(); change('before-new-db')
        command(cmd='info_edit_input',action='new_db_dialog')
        command(cmd='info_edit_input',action='new_db_name',text='qa-info-'+uuid.uuid4().hex[:8])
        settled(); capture('new-db-dialog')
        old_path=command(cmd='get_state')['db_path']
        command(cmd='info_edit_input',action='new_db_create')
        pending=settled()
        assert pending['awaiting_save'] and command(cmd='get_state')['db_path']==old_path,pending
        capture('new-db-save-required','info.saveYes')
        current=click('info.saveYes')
        new_state=wait_for(lambda:command(cmd='get_state'),
            lambda s:not s['db_switch_in_progress'] and s['db_path']!=old_path)
        new_path=Path(new_state['db_path'])
        assert new_path.is_file() and new_path.resolve().is_relative_to(args.isolation_root.resolve()),new_state
        with sqlite3.connect(databases[0]) as saved:
            actual=saved.execute('SELECT description FROM audio_files WHERE id=?',
                                 (current['edit_id'],)).fetchone()[0]
        assert actual==current['draft_description'],(actual,current)
        checks.append({'case':'new-db-waits-for-save-then-creates','state':current,
                       'created_db_path':str(new_path),'old_db_path':old_path})
        return
    select(0); before_exit=double_click(); change('exit-discard')
    exit_action='tray_exit' if args.edit_tray_suite else 'update_exit' if args.edit_update_suite else 'exit'
    command(cmd='info_edit_input',action=exit_action)
    pending=settled()
    assert pending['awaiting_save'] and process.poll() is None,pending
    if args.edit_tray_suite:
        assert not pending['window_iconic'],pending
        capture('tray-exit-choice-visible','info.saveNo')
    else:
        capture('exit-requires-choice','info.saveNo')
    for phase in ['hover','down','up']:
        command(cmd='info_page_pointer',key='info.saveNo',phase=phase)
        time.sleep(.12)
    process.wait(timeout=20)
    with sqlite3.connect(databases[0]) as saved:
        actual=saved.execute('SELECT description FROM audio_files WHERE id=?',(before_exit['edit_id'],)).fetchone()[0]
    assert actual==before_exit['saved_description'],(actual,before_exit)
    checks.append({'case':'tray-exit-restores-save-choice' if args.edit_tray_suite else
                   'update-exit-waits-for-explicit-no' if args.edit_update_suite else 'exit-waits-for-explicit-no',
                   'exit_code':process.returncode})
