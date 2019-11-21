package ru.yandex.spark.launcher

import com.google.common.net.HostAndPort
import com.twitter.scalding.Args
import org.apache.log4j.Logger
import ru.yandex.spark.discovery.{CypressDiscoveryService, DiscoveryService}
import ru.yandex.spark.yt.utils.YtClientConfiguration

import scala.annotation.tailrec
import scala.concurrent.duration._
import scala.language.postfixOps

object MasterLauncher extends App {
  private val log = Logger.getLogger(getClass)
  val masterArgs = MasterLauncherArgs(args)
  val discoveryService = new CypressDiscoveryService(masterArgs.ytConfig, masterArgs.discoveryPath)

  try {
    log.info("Start master")
    val (masterAddress, masterThread) = SparkLauncher.startMaster(masterArgs.port, masterArgs.webUiPort, masterArgs.opts)
    val masterAlive = discoveryService.waitAlive(masterAddress.hostAndPort, (5 minutes).toMillis)
    if (!masterAlive) {
      throw new RuntimeException("Master is not started")
    }
    log.info(s"Master started at port ${masterAddress.port}")

    log.info("Register master")
    discoveryService.register(masterArgs.id, masterArgs.operationId, masterAddress)
    log.info("Master registered")

    try {
      DiscoveryService.checkPeriodically(masterThread.isAlive)
      log.info("Master thread is not alive!")
    } finally {
      log.info("Removing master address")
      discoveryService.removeAddress(masterArgs.id)
    }
  } finally {
    log.info("Closing discovery service")
    discoveryService.close()
    log.info("Discovery service closed")
    log.info(s"Threads: ${Thread.activeCount()}")
  }
}

case class MasterLauncherArgs(id: String,
                              port: Int,
                              webUiPort: Int,
                              ytConfig: YtClientConfiguration,
                              discoveryPath: String,
                              operationId: String,
                              opts: Option[String])

object MasterLauncherArgs {
  def apply(args: Args): MasterLauncherArgs = MasterLauncherArgs(
    args.required("id"),
    args.optional("port").map(_.toInt).getOrElse(7077),
    args.optional("web-ui-port").map(_.toInt).getOrElse(8080),
    YtClientConfiguration(args.optional),
    args.optional("discovery-path").getOrElse(sys.env("SPARK_DISCOVERY_PATH")),
    args.required("operation-id"),
    args.optional("opts").map(_.drop(1).dropRight(1))
  )

  def apply(args: Array[String]): MasterLauncherArgs = MasterLauncherArgs(Args(args))
}
