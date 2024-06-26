#!/bin/bash -l

set -exo pipefail

CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TOP_DIR=${CWDIR}/../../../
source "${TOP_DIR}/gpdb_src/concourse/scripts/common.bash"
source "${TOP_DIR}/pljava_src/concourse/scripts/common.bash"
# for centos and suse, the compiled GPHOME is /usr/local/greenplum-db-devel
# but for compiled ubuntu16, it is /usr/local/gpdb. ;-(
gphome=${INSTALL_DIR:-/usr/local/cloudberry-db-devel}

function install_openssl(){
    pushd /opt
    wget --no-check-certificate https://www.openssl.org/source/openssl-1.0.2l.tar.gz
    wget --no-check-certificate https://www.openssl.org/source/openssl-fips-2.0.16.tar.gz
    wget --no-check-certificate https://ftp.nluug.nl/security/OpenSSH/openssh-6.9p1.tar.gz
    tar -zxf openssl-1.0.2l.tar.gz
    tar -zxf openssl-fips-2.0.16.tar.gz
    tar -zxf openssh-6.9p1.tar.gz

    source /opt/gcc_env.sh

    pushd openssl-fips-2.0.16
    ./config
    make
    make install
    popd
    pushd openssl-1.0.2l
    ./config --prefix=/usr/local/ssl --openssldir=/usr/local/ssl/ssl shared fips enable-ssl2
    make depend
    make
    make install
    popd

    cp -r /usr/local/ssl/fips-2.0/include/openssl/fips*.h /usr/local/ssl/include/openssl/
    rm -r /usr/local/ssl/fips-2.0

    #rpm -e openssh-6.6p1-4.7 --nodeps
    pushd openssh-6.9p1
    ./configure --prefix=/usr/ --sysconfdir=/etc/ssh --with-zlib --with-ssl-dir=/usr/local/ssl --with-md5-passwords mandir=/usr/share/man
    make
    make install
    cp contrib/suse/rc.sshd /etc/init.d/sshd
    chmod +x /etc/init.d/sshd
    cp -f -r sshd_config /etc/ssh/sshd_config
    cp -f -r sshd /usr/sbin/sshd
    cp -f -r ssh /usr/bin/ssh
    /etc/init.d/sshd restart
    popd

    popd

}

function prepare_test(){

	cat > /home/gpadmin/test.sh <<-EOF
		set -exo pipefail

        if [ "$OSVER" == "suse11" ]; then
            # Official GPDB for SUSE 11 comes with very old version of glibc, getting rid of it here
            unset LD_LIBRARY_PATH
        fi

        source ${TOP_DIR}/gpdb_src/gpAux/gpdemo/gpdemo-env.sh
        source $gphome/greenplum_path.sh
        gppkg -i pljava_gppkg/pljava-*.gppkg
        source $gphome/greenplum_path.sh
        gpstop -arf

        pushd pljava_src

        make targetcheck
        pushd gpdb/tests
        [ -s regression.diffs ] && cat regression.diffs && exit 1
        popd
        popd

	EOF

	chown -R gpadmin:gpadmin $(pwd)
	chown gpadmin:gpadmin /home/gpadmin/test.sh
	chmod a+x /home/gpadmin/test.sh

}

function run_test_pl() {
	su gpadmin -c "bash /home/gpadmin/test.sh $(pwd)"
}

function setup_gpadmin_user() {
	${TOP_DIR}/gpdb_src/concourse/scripts/setup_gpadmin_user.bash
}

function _main() {
    time prep_env
    time install_gpdb
    time setup_gpadmin_user

    time make_cluster
    time prepare_test
    time run_test_pl

}

_main "$@"
