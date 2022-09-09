groupadd -o -g $HOST_DOCKER_GROUP_ID host_docker
useradd -M -d $HOME -g host_docker -G docker -o -u $UID -s /bin/sh $USER

mkdir -p $HOME/.m2 $HOME/.sbt $HOME/.cache $HOME/.ivy2
chown -R $USER $HOME/.m2 $HOME/.sbt $HOME/.cache $HOME/.ivy2 /cache

export YT_TOKEN=${YT_TOKEN:-non_existent_token}
export SPARK_HOME=/app/spark-over-yt/.tox/py27/lib/python2.7/site-packages/pyspark
mkdir $HOME/.yt
echo $YT_TOKEN > $HOME/.yt/token

export SBT_CREDENTIALS=$HOME/.sbt/.credentials

# generate XML credentials
if [ -f $SBT_CREDENTIALS ]; then
    python3 /app/spark-over-yt/tools/teamcity/cli.py generate-xml-creds $SBT_CREDENTIALS $HOME/.m2/settings.xml
fi

SBT="sbt -Duser.home=/app --sbt-dir /cache/sbt --sbt-boot /cache/sbt/boot --ivy /cache/ivy"

cd $PROJECT_ROOT

case $SBT_COMMAND in
test)
    command="$SBT test"
    ;;
e2e)
    case $SPYT_PUBLISH_MODE in
    cluster)
        publish_command="$SBT -Dproxies=hume spytPublishClusterSnapshot"
        ;;
    spark-fork)
        publish_command="$SBT -Dproxies=hume spytPublishSparkForkSnapshot"
        ;;
    *)
        publish_command="echo Unknown publish mode $SPYT_PUBLISH_MODE && false"
        ;;
    esac
    command="$publish_command && $SBT -Dproxies=hume -DdiscoveryPath=//home/spark/e2e-new/spark-test e2e-test/e2eFullCircleTest"
    ;;
custom)
    case $SPYT_PUBLISH_MODE in
    client)
        command="$SBT spytPublishClientSnapshot && python3 /app/spark-over-yt/tools/teamcity/cli.py teamcity-report client"
        ;;
    cluster)
        command="$SBT spytPublishClusterSnapshot && python3 /app/spark-over-yt/tools/teamcity/cli.py teamcity-report client cluster"
        ;;
    spark-fork)
        command="$SBT spytPublishSparkForkSnapshot && python3 /app/spark-over-yt/tools/teamcity/cli.py teamcity-report client cluster spark-fork"
        ;;
    *)
        command="echo Unknown publish mode $SPYT_PUBLISH_MODE && false"
        ;;
    esac
    ;;
release)
        case $SPYT_PUBLISH_MODE in
    client)
        command="$SBT test && $SBT spytPublishSparkForkRelease && python3 /app/spark-over-yt/tools/teamcity/cli.py teamcity-report client "
        ;;
    cluster)
        command="$SBT test && $SBT spytPublishSparkForkRelease && python3 /app/spark-over-yt/tools/teamcity/cli.py teamcity-report client cluster"
        ;;
    spark-fork)
        command="$SBT test && $SBT spytPublishSparkForkRelease && python3 /app/spark-over-yt/tools/teamcity/cli.py teamcity-report client cluster spark-fork"
        ;;
    *)
        command="echo Unknown publish mode $SPYT_PUBLISH_MODE && false"
        ;;
    esac
    ;;
*)
    command="echo Unknown command $SBT_COMMAND && false"
    ;;
esac

echo Running command "$command" as $USER

su --shell /bin/bash --preserve-environment --command "$command" $USER

code=$?
su --shell /bin/bash --preserve-environment --command 'find -type d -name test-reports -exec cp -r --parents {} /reports \;' $USER

return $code
