package ru.yandex.spark.launcher

import com.twitter.scalding.Args
import org.apache.log4j.Logger
import ru.yandex.spark.discovery.SparkConfYsonable
import ru.yandex.spark.launcher.rest.MasterWrapperLauncher
import ru.yandex.spark.yt.wrapper.client.YtClientConfiguration

import scala.concurrent.duration._
import scala.language.postfixOps

object MasterLauncher extends App with VanillaLauncher with SparkLauncher with MasterWrapperLauncher {
  private val log = Logger.getLogger(getClass)
  val masterArgs = MasterLauncherArgs(args)
  import masterArgs._

  withDiscovery(ytConfig, discoveryPath) { discoveryService =>
    log.info("Start master")
    withService(startMaster) { master =>
      log.info("Start byop discovery service")
      withService(startMasterWrapper(args, master)) { masterWrapper =>
        master.waitAndThrowIfNotAlive(5 minutes)
        masterWrapper.waitAndThrowIfNotAlive(5 minutes)

        log.info("Register master")
        discoveryService.register(
          operationId,
          master.masterAddress,
          clusterVersion,
          masterWrapper.address,
          SparkConfYsonable(sparkSystemProperties)
        )
        log.info("Master registered")

        checkPeriodically(master.isAlive(3))
        log.error("Master is not alive")
      }
    }
  }
}

case class MasterLauncherArgs(ytConfig: YtClientConfiguration,
                              discoveryPath: String,
                              operationId: String,
                              clusterVersion: String)

object MasterLauncherArgs {
  def apply(args: Args): MasterLauncherArgs = MasterLauncherArgs(
    YtClientConfiguration(args.optional),
    args.optional("discovery-path").getOrElse(sys.env("SPARK_DISCOVERY_PATH")),
    args.optional("operation-id").getOrElse(sys.env("YT_OPERATION_ID")),
    args.optional("cluster-version").getOrElse(sys.env("SPARK_CLUSTER_VERSION"))
  )

  def apply(args: Array[String]): MasterLauncherArgs = MasterLauncherArgs(Args(args))
}
