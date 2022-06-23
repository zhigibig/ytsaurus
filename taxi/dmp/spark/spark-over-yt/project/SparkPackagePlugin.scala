package spyt

import com.typesafe.sbt.packager.linux.LinuxPackageMapping
import sbt.Keys._
import sbt.PluginTrigger.NoTrigger
import sbt._
import spyt.SparkPaths._

import java.io.{FileInputStream, FilenameFilter, InputStreamReader}
import java.util.Properties
import scala.annotation.tailrec

object SparkPackagePlugin extends AutoPlugin {
  override def requires = super.requires && YtPublishPlugin

  override def trigger = NoTrigger

  import YtPublishPlugin.autoImport._
  import autoImport._

  object autoImport {
    val sparkPackage = taskKey[File]("Build spark and add custom files")
    val sparkRebuild = taskKey[Unit]("Rebuild spark dist")
    val sparkAddCustomFiles = taskKey[File]("Add custom files to spark dist")

    val sparkHome = settingKey[File]("")
    val sparkForkHome = settingKey[File]("spark-fork directory")
    val sparkVersionPyFile = settingKey[File]("")

    val sparkAdditionalJars = taskKey[Seq[File]]("Jars to copy in SPARK_HOME/jars")
    val sparkAdditionalBin = settingKey[Seq[File]]("Scripts to copy in SPARK_HOME/bin")
    val sparkAdditionalPython = settingKey[Seq[File]]("Files to copy in SPARK_HOME/python")
    val sparkLocalConfigs = taskKey[Seq[File]]("Configs to copy in SPARK_HOME/conf")

    val sparkYtConfigs = taskKey[Seq[YtPublishArtifact]]("Configs to copy in YT conf dir")
    val sparkYtBinBasePath = taskKey[String]("YT base path for spark binaries")
    val sparkYtSubdir = taskKey[String]("Snapshots or releases")
    val sparkIsSnapshot = settingKey[Boolean]("Flag of spark snapshot version")
    val sparkReleaseGlobalConfig = settingKey[Boolean]("If true, global config will be rewritten, default is !sparkIsSnapshot")
    val sparkReleaseLinks = settingKey[Boolean]("If true, links in //sys/spark will be created, default is !sparkIsSnapshot")
    val sparkYtServerProxyPath = settingKey[Option[String]]("YT path of ytserver-proxy binary")

    val sparkMvnInstall = taskKey[Unit]("")
    val sparkMvnDeploy = taskKey[Unit]("")

    def createPackageMapping(src: File, dst: String): LinuxPackageMapping = {

      @tailrec
      def inner(src: Seq[(File, String)], result: Seq[(File, String)]): Seq[(File, String)] = {
        src.headOption match {
          case None => result
          case Some((f, d)) if f.isFile => inner(src.tail, (f, d) +: result)
          case Some((f, d)) if f.isDirectory =>
            val children = f.listFiles().map(f => f -> s"$d/${f.name}")
            inner(src.tail ++ children, (f, d) +: result)
        }
      }

      LinuxPackageMapping(inner(Seq(src -> dst), Nil))
    }
  }

  private def readSparkDefaults(file: File): Map[String, String] = {
    import scala.collection.JavaConverters._
    val reader = new InputStreamReader(new FileInputStream(file))
    val properties = new Properties()
    try {
      properties.load(reader)
    } finally {
      reader.close()
    }
    properties.stringPropertyNames().asScala.map { name =>
      name -> properties.getProperty(name)
    }.toMap
  }

  override def projectSettings: Seq[Def.Setting[_]] = super.projectSettings ++ Seq(
    sparkHome := (ThisBuild / baseDirectory).value.getParentFile / "spark",
    sparkForkHome := (ThisBuild / baseDirectory).value / "spark-fork",
    (ThisBuild / sparkVersionPyFile) := sparkHome.value / "python" / "pyspark" / "version.py",
    sparkIsSnapshot := isSnapshot.value || version.value.contains("beta") || version.value.contains("dev"),
    sparkReleaseGlobalConfig := !sparkIsSnapshot.value,
    sparkReleaseLinks := !sparkIsSnapshot.value,
    sparkLocalConfigs := {
      Seq(
        sparkForkHome.value / "spark-defaults.conf",
        sparkForkHome.value / "spark-env.sh",
        sparkForkHome.value / "metrics.properties",
        sparkForkHome.value / "log4j" / "log4j.properties",
        sparkForkHome.value / "log4j" / "log4j.workerLogJson.properties",
        sparkForkHome.value / "log4j" / "log4j.worker.properties"
      )
    },
    sparkAdditionalBin := Nil,
    sparkYtSubdir := {
      if (sparkIsSnapshot.value) "snapshots" else "releases"
    },
    sparkYtBinBasePath := s"$sparkYtBasePath/bin/${sparkYtSubdir.value}/${version.value}",
    sparkYtServerProxyPath := {
      Option(System.getProperty("proxyVersion")).map(version =>
        s"$defaultYtServerProxyPath-$version"
      )
    },
    sparkYtConfigs := {
      val log = streams.value.log

      val binBasePath = sparkYtBinBasePath.value
      val sparkVersion = version.value
      val versionConfPath = s"$sparkYtConfPath/${sparkYtSubdir.value}/$sparkVersion"

      val sidecarConfigs = ((Compile / resourceDirectory).value / "config").listFiles()
      val sidecarConfigsClusterPaths = sidecarConfigs.map(file => s"$versionConfPath/${file.getName}")

      val launchConfig = SparkLaunchConfig(
        binBasePath,
        ytserver_proxy_path = sparkYtServerProxyPath.value,
        file_paths = Seq(
          s"$binBasePath/spark.tgz",
          s"$binBasePath/spark-yt-launcher.jar"
        ) ++ sidecarConfigsClusterPaths
      )
      val launchConfigPublish = YtPublishDocument(
        launchConfig,
        versionConfPath,
        None,
        "spark-launch-conf",
        sparkIsSnapshot.value
      )
      val configsPublish = sidecarConfigs.map(file => YtPublishFile(file, versionConfPath, None, isTtlLimited = sparkIsSnapshot.value))

      val globalConfigPublish = if (sparkReleaseGlobalConfig.value) {
        log.info(s"Prepare configs for ${ytProxies.mkString(", ")}")
        ytProxies.map { proxy =>
          val proxyShort = proxy.split("\\.").head
          val proxyDefaultsFile = (Compile / resourceDirectory).value / s"spark-defaults-$proxyShort.conf"
          val proxyDefaults = readSparkDefaults(proxyDefaultsFile)
          val globalConfig = SparkGlobalConfig(proxyDefaults, sparkVersion)

          YtPublishDocument(globalConfig, sparkYtConfPath, Some(proxy), "global", sparkIsSnapshot.value)
        }
      } else Nil

      configsPublish ++ (launchConfigPublish +: globalConfigPublish)
    },
    sparkAddCustomFiles := {
      val sparkDist = sparkHome.value / "dist"
      sparkAdditionalJars.value.foreach { file =>
        IO.copyFile(file, sparkDist / "jars" / file.name)
      }
      sparkLocalConfigs.value.foreach { file =>
        IO.copyFile(file, sparkDist / "conf" / file.name)
      }
      sparkAdditionalBin.value.foreach { file =>
        IO.copyFile(file, sparkDist / "bin" / file.name, preserveExecutable = true)
      }
      val pythonDir = sparkDist / "bin" / "python"
      if (!pythonDir.exists()) IO.createDirectory(pythonDir)
      val ignorePython = Set("build", "dist", ".egg-info", "setup.py", ".pyc", "__pycache__")
      sparkAdditionalPython.value.foreach(FileUtils.copyDirectory(_, pythonDir, ignorePython))
      sparkDist
    },
    sparkRebuild := {
      val log = streams.value.log
      val sparkDist = sparkHome.value / "dist"
      val rebuildSpark = Option(System.getProperty("rebuildSpark")).forall(_.toBoolean) || !sparkDist.exists()
      log.info(s"System property rebuildSpark=${Option(System.getProperty("rebuildSpark"))}," +
        s"spark dist ${if (sparkDist.exists()) "already exists" else "doesn't exist"}")
      if (rebuildSpark) {
        buildSpark(sparkHome.value.toString)
      } else {
        FileUtils.deleteFiles(sparkDist / "jars", new FilenameFilter {
          override def accept(dir: File, name: String): Boolean = name.startsWith("spark-yt-")
        })
      }
    },
    sparkPackage := Def.sequential(
      sparkRebuild,
      sparkAddCustomFiles
    ).value,
    sparkMvnInstall := {
      mvnInstall(sparkHome.value)
    },
    sparkMvnDeploy := {
      mvnDeploy(sparkHome.value)
    }
  )

  private def buildSpark(sparkHome: String): Unit = {
    import scala.language.postfixOps
    import scala.sys.process._

    val sparkBuildCommand = s"$sparkHome/dev/make-distribution.sh -Phadoop-2.7"
    println("Building spark...")
    val code = (sparkBuildCommand !)

    if (code != 0) {
      throw new RuntimeException("Spark build failed")
    }
    println("Spark build completed")
  }

  def mvnInstall(sparkHome: File): Unit = {
    import scala.language.postfixOps
    import scala.sys.process._

    val sparkBuildCommand = s"mvn -P scala-2.12 clean install -Dscala-2.12 -Djava11 -DskipTests=true -pl core -pl sql/catalyst -pl sql/core"
    println("Building spark...")
    val code = Process(sparkBuildCommand, cwd = sparkHome) !

    if (code != 0) {
      throw new RuntimeException("Spark installation failed")
    }
    println("Spark installation completed")
  }

  def mvnDeploy(sparkHome: File): Unit = {
    import scala.language.postfixOps
    import scala.sys.process._

    val sparkBuildCommand = s"mvn -P scala-2.12 clean deploy -Dscala-2.12 -Djava11 -DskipTests=true -pl core -pl sql/catalyst -pl sql/core"
    println("Building spark...")
    val code = Process(sparkBuildCommand, cwd = sparkHome) !

    if (code != 0) {
      throw new RuntimeException("Spark installation failed")
    }
    println("Spark installation completed")
  }
}
