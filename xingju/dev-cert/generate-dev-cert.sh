#!/bin/bash

set -e

# 📂 初始化目录结构
mkdir -p dev-ca/{private,certs,newcerts}
touch dev-ca/index.txt
echo 1000 > dev-ca/serial

# 🧾 CA 配置文件
cat > dev-ca/openssl.cnf <<EOF
[ ca ]
default_ca = my_ca

[ my_ca ]
dir               = ./dev-ca
certs             = \$dir/certs
new_certs_dir     = \$dir/newcerts
database          = \$dir/index.txt
serial            = \$dir/serial
private_key       = \$dir/private/ca.key.pem
certificate       = \$dir/certs/ca.cert.pem
default_md        = sha256
policy            = policy_strict
default_days      = 3650
x509_extensions   = v3_ca

[ policy_strict ]
commonName              = supplied

[ req ]
default_bits        = 2048
default_md          = sha256
prompt              = no
distinguished_name  = req_distinguished_name
x509_extensions     = v3_ca

[ req_distinguished_name ]
CN = Dev CA

[ v3_ca ]
basicConstraints = critical,CA:TRUE
keyUsage = critical, digitalSignature, cRLSign, keyCertSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always,issuer
EOF

# 🔐 创建 CA 密钥与根证书
openssl genrsa -out dev-ca/private/ca.key.pem 2048
openssl req -x509 -new -nodes -key dev-ca/private/ca.key.pem \
    -days 3650 -out dev-ca/certs/ca.cert.pem \
    -config dev-ca/openssl.cnf

echo "✅ CA 证书生成完成：dev-ca/certs/ca.cert.pem"

# 📄 服务端证书请求配置（含 SAN）
cat > server.cnf <<EOF
[req]
default_bits = 2048
prompt = no
default_md = sha256
req_extensions = req_ext
distinguished_name = dn

[dn]
CN = dev.myubuntu.com

[req_ext]
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = dev.myubuntu.com
DNS.3 = myubuntu.com
DNS.4 = dev.antsentinel.com
DNS.5 = test.antsentinel.com
IP.1 = 127.0.0.1
IP.2 = 192.168.1.176
IP.3 = 192.168.1.198
EOF

# 🔐 创建服务端私钥与 CSR
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -config server.cnf

# 📄 扩展配置（用于签发证书）
cat > server-ext.cnf <<EOF
[ server_ext ]
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = dev.myubuntu.com
IP.1 = 192.168.1.176
EOF

# 🧾 使用 CA 签发服务端证书
openssl ca -config dev-ca/openssl.cnf \
    -extensions server_ext \
    -extfile server-ext.cnf \
    -in server.csr -out server.crt \
    -batch -notext

echo "✅ 服务端证书生成完成：server.crt / server.key"

