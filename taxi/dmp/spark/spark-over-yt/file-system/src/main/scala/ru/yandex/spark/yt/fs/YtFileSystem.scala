package ru.yandex.spark.yt.fs

import java.io.FileNotFoundException

import org.apache.hadoop.fs._
import org.apache.hadoop.fs.permission.FsPermission
import org.apache.hadoop.util.Progressable
import org.apache.log4j.Logger
import ru.yandex.spark.yt.wrapper.YtWrapper
import ru.yandex.spark.yt.wrapper.cypress.PathType
import ru.yandex.yt.ytclient.proxy.YtClient

import scala.language.postfixOps

@SerialVersionUID(1L)
class YtFileSystem extends YtFileSystemBase {
  private val log = Logger.getLogger(getClass)

  override def listStatus(f: Path): Array[FileStatus] = {
    log.debugLazy(s"List status $f")
    implicit val ytClient: YtClient = yt
    val path = ytPath(f)
    val pathType = YtWrapper.pathType(path)

    pathType match {
      case PathType.File => Array(getFileStatus(f))
      case PathType.Directory => listYtDirectory(f, path, None)
      case _ => throw new IllegalArgumentException(s"Can't list $pathType")
    }
  }

  override def getFileStatus(f: Path): FileStatus = {
    log.debugLazy(s"Get file status $f")
    implicit val ytClient: YtClient = yt
    val path = ytPath(f)

    if (!YtWrapper.exists(path)) {
      throw new FileNotFoundException(s"File $path is not found")
    } else {
      val pathType = YtWrapper.pathType(path)
      pathType match {
        case PathType.File => new FileStatus(YtWrapper.fileSize(path), false, 1, 0, 0, f)
        case PathType.Directory => new FileStatus(0, true, 1, 0, 0, f)
        case PathType.None => null
      }
    }
  }

  override def create(f: Path, permission: FsPermission, overwrite: Boolean, bufferSize: Int,
                      replication: Short, blockSize: Long, progress: Progressable): FSDataOutputStream = {
    create(f, permission, overwrite, bufferSize, replication, blockSize, progress, statistics)
  }

  override def mkdirs(f: Path, permission: FsPermission): Boolean = {
    implicit val ytClient: YtClient = yt
    val path = ytPath(f)
    YtWrapper.createDir(path, ignoreExisting = true)
    true
  }
}
