package ru.yandex.spark.yt.submit

import com.google.common.util.concurrent.ThreadFactoryBuilder
import io.netty.channel.DefaultEventLoopGroup
import org.apache.commons.io.FileUtils
import org.apache.spark.deploy.rest.{DriverState, MasterClient, RestSubmissionClientWrapper}
import org.apache.spark.launcher.InProcessLauncher
import org.slf4j.LoggerFactory
import ru.yandex.inside.yt.kosher.cypress.YPath
import ru.yandex.spark.yt.wrapper.discovery.CypressDiscoveryService

import java.io.File
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import scala.annotation.tailrec
import scala.concurrent.duration._
import scala.language.postfixOps
import scala.util.{Failure, Try}

class SubmissionClient(proxy: String,
                       discoveryPath: String,
                       spytVersion: String,
                       user: String,
                       token: String) {
  private val log = LoggerFactory.getLogger(getClass)

  private val eventLogPath = CypressDiscoveryService.eventLogPath(discoveryPath)

  private val cluster = new AtomicReference[SparkCluster](SparkCluster.get(proxy, discoveryPath, user, token))

  private val loop = new DefaultEventLoopGroup(1, new ThreadFactoryBuilder().setDaemon(true).build()).next()
  loop.scheduleAtFixedRate(() => updateCluster(), 0, 5, TimeUnit.MINUTES)

  def newLauncher(): InProcessLauncher = new InProcessLauncher()

  // for Java
  def submit(launcher: InProcessLauncher): Try[String] = {
    submit(launcher, RetryConfig())
  }

  def submit(launcher: InProcessLauncher,
             retryConfig: RetryConfig): Try[String] = {
    launcher.setDeployMode("cluster")

    launcher.setConf("spark.master.rest.enabled", "true")
    launcher.setConf("spark.master.rest.failover", "false")
    launcher.setConf("spark.rest.client.awaitTermination.enabled", "false")

    launcher.setConf("spark.hadoop.yt.proxy", proxy)
    launcher.setConf("spark.hadoop.yt.user", user)
    launcher.setConf("spark.hadoop.yt.token", token)

    launcher.setConf("spark.yt.version", spytVersion)
    launcher.setConf("spark.yt.jars", s"yt:/${spytJarPath(spytVersion)}")
    launcher.setConf("spark.yt.pyFiles", s"yt:/${spytPythonPath(spytVersion)}")
    launcher.setConf("spark.eventLog.dir", "ytEventLog:/" + eventLogPath)

    submitInner(launcher, retryConfig)
  }

  def getStatus(id: String): DriverState = {
    tryToGetStatus(id).getOrElse(DriverState.UNDEFINED)
  }

  def getStringStatus(id: String): String = {
    getStatus(id).name()
  }

  private def tryToGetStatus(id: String): Try[DriverState] = {
    val res = Try {
      val response = RestSubmissionClientWrapper.requestSubmissionStatus(cluster.get().client, id)
      if (response.success) {
        DriverState.valueOf(response.driverState)
      } else {
        DriverState.UNKNOWN // master restarted and can't find this submission id
      }
    }
    if (res.isFailure) {
      log.warn(s"Failed to get status of submission $id")
      forceClusterUpdate()
    }
    res
  }

  def getActiveDrivers: Seq[String] = {
    val response = MasterClient.activeDrivers(cluster.get().master)
    if (response.isFailure) {
      log.warn(s"Failed to get list of active drivers")
    }
    response.getOrElse(Nil)
  }

  def kill(id: String): Boolean = {
    val response = RestSubmissionClientWrapper.killSubmission(cluster.get().client, id)
    response.success.booleanValue()
  }

  case class SubmissionFiles(id: File, error: File) {
    def delete(): Unit = {
      if (id.exists()) id.delete()
      if (error.exists()) error.delete()
    }
  }

  private def updateCluster(): Unit = {
    log.debug(s"Update cluster addresses from $discoveryPath")
    cluster.set(SparkCluster.get(proxy, discoveryPath, user, token))
  }

  private def forceClusterUpdate(): Unit = {
    loop.submit(new Runnable {
      override def run(): Unit = updateCluster()
    })
  }

  private def configureCluster(launcher: InProcessLauncher): Unit = {
    val clusterValue = cluster.get()
    launcher.setMaster(clusterValue.master)
    launcher.setConf("spark.rest.master", clusterValue.masterRest)
    launcher.setConf("spark.yt.cluster.version", clusterValue.version)
  }

  private def prepareSubmissionFiles(launcher: InProcessLauncher): SubmissionFiles = {
    val fileName = s"spark-submission-${UUID.randomUUID()}"
    val idFile = new File(FileUtils.getTempDirectory, s"$fileName-id")
    val errorFile = new File(FileUtils.getTempDirectory, s"$fileName-error")
    launcher.setConf("spark.rest.client.submissionIdFile", idFile.getAbsolutePath)
    launcher.setConf("spark.rest.client.submissionErrorFile", errorFile.getAbsolutePath)
    SubmissionFiles(idFile, errorFile)
  }

  @tailrec
  private def submitInner(launcher: InProcessLauncher,
                          retryConfig: RetryConfig = RetryConfig(),
                          retry: Int = 1): Try[String] = {
    val submissionFiles = prepareSubmissionFiles(launcher)
    val submissionId = Try {
      configureCluster(launcher)
      launcher.startApplication()
      getSubmissionId(submissionFiles)
    }
    submissionFiles.delete()

    submissionId match {
      case Failure(e) if !retryConfig.enableRetry =>
        forceClusterUpdate()
        Failure(new RuntimeException("Failed to submit job and retry is disabled", e))
      case Failure(e) if retry >= retryConfig.retryLimit =>
        forceClusterUpdate()
        Failure(new RuntimeException(s"Failed to submit job and retry limit ${retryConfig.retryLimit} exceeded", e))
      case Failure(e) =>
        forceClusterUpdate()
        log.warn("Failed to submit job and retry is enabled")
        log.warn(e.getMessage)
        log.info(s"Retry to submit job in ${retryConfig.retryInterval.toCoarsest}")
        Thread.sleep(retryConfig.retryInterval.toMillis)
        submitInner(launcher, retryConfig, retry + 1)
      case success => success
    }
  }

  private def getSubmissionId(submissionFiles: SubmissionFiles): String = {
    while (!submissionFiles.id.exists() && !submissionFiles.error.exists()) {
      log.debug(s"Waiting for submission id in file: ${submissionFiles.id}")
      Thread.sleep((2 seconds).toMillis)
    }
    if (submissionFiles.error.exists()) {
      val message = FileUtils.readFileToString(submissionFiles.error)
      throw new RuntimeException(s"Spark submission finished with error: $message")
    }
    FileUtils.readFileToString(submissionFiles.id)
  }

  private val SPARK_BASE_PATH = YPath.simple("//home/spark")
  private val SPYT_BASE_PATH = SPARK_BASE_PATH.child("spyt")
  private val RELEASES_SUBDIR = "releases"
  private val SNAPSHOTS_SUBDIR = "snapshots"

  private def spytJarPath(spytVersion: String): YPath = {
    getSpytVersionPath(spytVersion).child("spark-yt-data-source.jar")
  }

  private def spytPythonPath(spytVersion: String): YPath = {
    getSpytVersionPath(spytVersion).child("spyt.zip")
  }

  private def getSpytVersionPath(spytVersion: String): YPath = {
    SPYT_BASE_PATH.child(versionSubdir(spytVersion)).child(spytVersion)
  }

  private def versionSubdir(version: String): String = {
    if (version.contains("SNAPSHOT") || version.contains("beta")) SNAPSHOTS_SUBDIR else RELEASES_SUBDIR
  }
}
