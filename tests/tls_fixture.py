from datetime import datetime, timedelta, timezone
import os
import shutil
import ssl
import subprocess


OPENSSL = os.environ.get('LYRA_TEST_OPENSSL') or shutil.which('openssl')


def tls_fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    config = root / 'ca.cnf'
    config.write_text('''[ca]
default_ca = local
[local]
database = index.txt
serial = serial
new_certs_dir = .
default_md = sha256
default_days = 2
policy = subject
[subject]
commonName = supplied
[root]
basicConstraints = critical,CA:true
keyUsage = critical,keyCertSign,cRLSign
[server]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
''', encoding='ascii')
    (root / 'index.txt').write_text('', encoding='ascii')
    (root / 'serial').write_text('01\n', encoding='ascii')

    def openssl(*arguments):
        result = subprocess.run([OPENSSL, *arguments], cwd=root, capture_output=True, timeout=20)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.decode())

    start = (datetime.now(timezone.utc) - timedelta(hours=1)).strftime('%y%m%d%H%M%SZ')
    subject = '/CN=Lyra local test root'
    openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', subject, '-keyout', 'ca.key', '-out', 'ca.csr')
    openssl('req', '-x509', '-key', 'ca.key', '-subj', subject, '-days', '2', '-out', 'bootstrap.pem')
    openssl('ca', '-batch', '-notext', '-config', config.name, '-selfsign', '-cert', 'bootstrap.pem', '-keyfile', 'ca.key',
            '-extensions', 'root', '-startdate', start, '-in', 'ca.csr', '-out', 'ca.pem')
    openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost', '-keyout', 'server.key', '-out', 'server.csr')
    openssl('ca', '-batch', '-notext', '-config', config.name, '-cert', 'ca.pem', '-keyfile', 'ca.key', '-extensions', 'server',
            '-startdate', start, '-in', 'server.csr', '-out', 'server.pem')
    return root / 'ca.pem', root / 'server.pem', root / 'server.key'


def trust_local(ca):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.load_verify_locations(cafile=str(ca))
    return context
