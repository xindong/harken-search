"""安装语言和已接入法务原稿的发布回归。实际窗口操作见同卡计划。"""
from pathlib import Path
import hashlib
import re

import pytest

ROOT = Path(__file__).resolve().parents[1]

def installer():
    return (ROOT / 'installer/HarkenSearch_Setup.iss').read_text(encoding='utf-8')

def test_installer_follows_windows_display_language_even_after_previous_install():
    text = installer()
    assert 'LanguageDetectionMethod=uilanguage' in text
    assert 'ShowLanguageDialog=no' in text
    assert 'UsePreviousLanguage=no' in text
    languages = re.findall(r'^Name: "([^"]+)"; MessagesFile: "([^"]+)"$', text, re.M)
    assert languages == [('english', 'compiler:Default.isl'),
                         ('chinesesimp', 'Languages' + chr(92) + 'ChineseSimplified.isl')]
    chinese = (ROOT / 'installer/Languages/ChineseSimplified.isl').read_text(encoding='utf-8-sig')
    assert 'LanguageID=$0804' in chinese
    assert 'SetupAppTitle=安装' in chinese

def test_first_agreement_is_product_eula_but_apache_license_is_still_shipped():
    text = installer()
    assert not re.search(r'^LicenseFile=', text, re.M)
    assert 'CreateCustomPage(wpWelcome,' in text
    assert r'Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion' in text

def test_interactive_agreements_require_consent_but_not_scrolling():
    text = installer()
    assert 'CheckControl.Checked := False;' in text
    assert 'MemoControl.ScrollBars := ssVertical;' in text
    assert 'LegalReadRequired' not in text
    assert 'IsLegalMemoAtBottom' not in text
    assert 'LegalCheckClick' not in text
    next_click = text.split('function NextButtonClick', 1)[1].split('\nend;', 1)[0]
    for document in ('Eula', 'Privacy'):
        assert f'if not {document}Loaded then' in next_click
        assert f'else if not {document}Check.Checked then' in next_click
    assert next_click.count("CustomMessage('LegalConsentRequired')") == 2

def test_silent_setup_refuses_to_install_without_consent_before_any_prompt():
    text = installer()
    initialize = text.split('function InitializeSetup', 1)[1].split('\nend;', 1)[0]
    consent_check, webview_check = initialize.split('if not RegQueryStringValue', 1)
    assert 'not ReadInstallerConsent' in consent_check
    assert 'Exit;' in consent_check
    assert "CustomMessage('WebViewMissing')" in webview_check

def test_consent_only_cannot_install_or_record_consent_silently():
    text = installer()
    initialize = text.split('function InitializeSetup', 1)[1].split('\nend;', 1)[0]
    prepare = text.split('function PrepareToInstall', 1)[1].split('\nend;', 1)[0]
    assert "HasSetupParam('/RECORDCONSENT')" in initialize
    assert 'if ConsentOnly and WizardSilent then' in initialize
    assert prepare.index('if ConsentOnly then') < prepare.index('AcquireInstallMutationMutex')
    next_click = text.split('function NextButtonClick', 1)[1].split('\nend;', 1)[0]
    assert next_click.index('not PrivacyCheck.Checked') < next_click.index('RecordInstallerConsent')
    assert 'EulaLoaded and EulaCheck.Checked' in next_click

def test_consent_receipt_is_scoped_and_consumed_before_installation():
    text = installer()
    fingerprint = text.split('function InstallerConsentFingerprint', 1)[1].split('\nend;', 1)[0]
    assert "GetSHA256OfFile(LegalPath('EULA'))" in fingerprint
    assert "GetSHA256OfFile(LegalPath('PRIVACY_POLICY'))" in fingerprint
    assert "GetSHA256OfFile(ExpandConstant('{srcexe}'))" in fingerprint
    assert 'ActiveLanguage' in fingerprint
    assert '{#MyAppVersion}' in fingerprint
    assert 'RegWriteStringValue(HKLM64, InstallerConsentKey, ConsentUserSid,' in text
    assert 'RegQueryStringValue(HKLM64, InstallerConsentKey, ConsentUserSid,' in text
    read = text.split('function ReadInstallerConsent', 1)[1].split('\nend;', 1)[0]
    assert 'IsDesktopUser(ConsentUserSid)' in read
    prepare = text.split('function PrepareToInstall', 1)[1].split('\nend;', 1)[0]
    assert 'not ConsumeInstallerConsent' in prepare

@pytest.mark.parametrize('name,sha,count', [
    ('EULA.en.md', 'e566c9003c93fc3c04ab2c73842263d28f8dcd3b55b3e3d34f00de3ba1ad7716', 116),
    ('PRIVACY_POLICY.en.md', 'bfe03c3a0c0824186d89a281fe299a552eba32be344374b3896e820f91256622', 67),
    ('OPEN_SOURCE_RELEASE_NOTICE.en.md', 'f123662304d8aec284fdcb2e7f1d94df1a907c1b33b22cf981de09bbf0b9c9fd', 67),
])
def test_english_documents_match_20261028_legal_original(name, sha, count):
    text = (ROOT / 'docs/legal' / name).read_text(encoding='utf-8')
    assert hashlib.sha256(text.encode()).hexdigest() == sha
    assert len([p for p in text.splitlines() if p.strip()]) == count
    assert 'October 28, 2026' in text
    assert 'PLACEHOLDER' not in text

def test_custom_messages_have_both_languages_and_no_untranslated_references():
    text = installer()
    section = text.split('[CustomMessages]', 1)[1].split('[Code]', 1)[0]
    pairs = re.findall(r'^(english|chinesesimp).([A-Za-z0-9_]+)=(.+)$', section, re.M)
    messages = {language: {key: value for lang, key, value in pairs if lang == language}
                for language in ('english', 'chinesesimp')}
    assert messages['english'].keys() == messages['chinesesimp'].keys()
    references = set(re.findall(r"CustomMessage\('([^']+)'\)", text))
    builtin = {'LaunchProgram'}
    chinese = (ROOT / 'installer/Languages/ChineseSimplified.isl').read_text(encoding='utf-8-sig')
    assert all(re.search(rf'^{key}=.+$', chinese, re.M) for key in builtin)
    assert references and references <= (messages['english'].keys() | builtin)
    for key, value in messages['english'].items():
        assert not re.search('[\u4e00-\u9fff]', value), key
        assert re.findall(r'%[1-9n]', value) == re.findall(r'%[1-9n]', messages['chinesesimp'][key]), key

def test_all_six_legal_documents_are_required_by_every_packaging_path():
    text = installer()
    qa = (ROOT / 'scripts/package_qa_exe.ps1').read_text(encoding='utf-8')
    workflow = (ROOT / '.github/workflows/package-update.yml').read_text(encoding='utf-8')
    for base in ('EULA', 'PRIVACY_POLICY', 'OPEN_SOURCE_RELEASE_NOTICE'):
        for suffix in ('.md', '.en.md'):
            name = base + suffix
            expected = rf'Source: "{{#SourceDir}}\docs\legal\{name}"; DestDir: "{{app}}\docs\legal"; Flags: ignoreversion'
            assert expected in text
            assert expected + ' skipifsourcedoesntexist' not in text
            assert rf'"docs\legal\{name}"' in qa
            assert workflow.count(f"'{name}'") == 2
