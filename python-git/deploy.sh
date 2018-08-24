#!/bin/bash -eux

CODENAME="$(lsb_release --short --codename)"

clean() {
    rm -rf yt/wrapper/tests.sandbox/* .pybuild *.egg-info
    python setup.py clean
    make -f debian/rules clean
}

found_version() {
    local package="$1"
    local repo="$2"
    local target_version="$3"
    for branch in "unstable" "testing" "stable"; do
        for version in $(
            curl -s "http://dist.yandex.ru/$repo/$branch/all/Packages.gz" \
                | zcat \
                | ./find_package.py $package); do
            if [ "$version" = "$target_version" ]; then
                echo 1
                return
            fi
        done
    done
    echo 0
}

init_vars() {
    set +u

    if [ -z "$FORCE_DEPLOY" ]; then
        FORCE_DEPLOY=""
    fi
    export FORCE_DEPLOY

    if [ -z "$FORCE_BUILD" ]; then
        FORCE_BUILD=""
    fi
    export FORCE_BUILD

    if [ -z "$SKIP_WHEEL" ]; then
        SKIP_WHEEL=""
    fi
    export SKIP_WHEEL

    if [ -z "$SKIP_DUPLOAD" ]; then
        SKIP_DUPLOAD=""
    fi
    export SKIP_DUPLOAD

    if [ -z "$CREATE_CONDUCTOR_TICKET" ]; then
        CREATE_CONDUCTOR_TICKET=""
    fi
    export CREATE_CONDUCTOR_TICKET

    if [ -z "$EXTRA_REPOSITORIES" ]; then
        EXTRA_REPOSITORIES=""
    fi
    export EXTRA_REPOSITORIES

    set -u
}

PACKAGE=$1
PACKAGE_PATH="$PACKAGE"

init_vars

# Copy package files to the python root
# NB: Symbolic links doesn't work correctly with `sdist upload`
cp -r -L $PACKAGE_PATH/debian $PACKAGE_PATH/setup.py .

# Initial cleanup
clean

VERSION=$(dpkg-parsechangelog | grep Version -m 1 | awk '{print $2}')

# Do not upload local version. If version endswith local it is assumed that
# it will be built and uploaded manually.
if [[ "$VERSION" == *local ]]; then
    rm -rf debian setup.py MANIFEST.in requirements.txt
    exit 0
fi

# Detect repos to upload.
REPOS=""
case $PACKAGE in
    yandex-yt-python|yandex-yt-python-tools|yandex-yt-local|yandex-yt-transfer-manager-client|yandex-yp-python|yandex-yp-python-skynet)
        REPOS="common yt-common search"
        ;;
    yandex-yt-python-fennel|yandex-yt-fennel)
        REPOS="yt-common"
        ;;
    yandex-yt-python-yson|yandex-yt-python-driver-rpc)
        REPOS="yandex-$CODENAME yt-$CODENAME"
        ;;
    yandex-yt-python-proto)
        REPOS="common yt-common"
        ;;
esac

REPOS="$REPOS $EXTRA_REPOSITORIES"

REPOS_TO_UPLOAD=""
if [ -z "$FORCE_DEPLOY" ]; then
    for REPO in $REPOS; do
        if [ "$(found_version "$PACKAGE" "$REPO" "$VERSION")" = "0" ]; then
            REPOS_TO_UPLOAD="$REPOS_TO_UPLOAD $REPO"
        fi
    done
else
    REPOS_TO_UPLOAD="$REPOS"
fi

# Build and upload debian package if necessary
if [ -n "$REPOS_TO_UPLOAD" ] || [ -n "$FORCE_BUILD" ]; then
    # NB: Never strip binaries and so-libraries.
    DEB_STRIP_EXCLUDE=".*" DEB=1 dpkg-buildpackage -i -I -rfakeroot

    if [ -z "$SKIP_DUPLOAD" ]; then
        # Upload debian package
        for REPO in $REPOS_TO_UPLOAD; do
            if [ "$REPO" = "common" ]; then
                # NB: used in postptocess.sh by some packages.
                export CREATE_CONDUCTOR_TICKET="true"
            fi
            dupload "../${PACKAGE}_${VERSION}_amd64.changes" --force --to $REPO
        done
    fi
fi

# Upload python wheel
if [ -z "$SKIP_WHEEL" ]; then
    # Wheels are tagged only with interpreter type and platform (e.g. win32, macosx, etc.)
    # and not linux distribution aware. To preserve binary compatibility with as many
    # Ubuntu distributions as possible wheel should be built only on the oldest
    # distribution (since all new distributions have backward compatibility).
    # See PEP-425, PEP-513 and https://github.com/pypa/manylinux for more details.
    # This is why oldest distributions are chosen - precise.
    if [ "$CODENAME" = "precise" ]; then
        python setup.py bdist_wheel --universal upload -r yandex
    fi
fi

# Some postprocess steps
if [ -f "$PACKAGE_PATH/postprocess.sh" ]; then
    $PACKAGE_PATH/postprocess.sh
fi


# Final cleanup
clean
rm -rf debian setup.py MANIFEST.in requirements.txt dist __pycache__ stable_versions
