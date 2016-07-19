#!/usr/bin/python

import yt.wrapper as yt
import yt.logger as logger
from email.mime.text import MIMEText
from subprocess import Popen, PIPE
import time

import StringIO

from argparse import ArgumentParser

yt.config["proxy"]["url"] = "locke"

def send_notification(cluster, notification_id, notification, recipients):
    logger.debug("Notification %s content:\n%s".format(notification_id, notification))
   
    description = notification.get("description", "No description")
    if not ("<" in description and ">" in description):
        # Seems like the author of description forgot about html-format, let's 
        # fix newlines for him.
        description = description.replace("\n", "<br />") 
    mail_subject = "[{0}] Notification: {1}".format(cluster, notification.get("title", "(no subject)"))
    mail_body = """<html>
    <head></head>
    <body>
        <p><b>Cluster</b>: {0}</p>
        <p>{1}</p>
        <p><b>Estimated start time</b>: {2}</p>
        <p><b>Estimated finish time</b>: {3}</p>
        <p><b>Created by</b>: {4}</p>
    </body>
</html>
""".format(
        cluster,
        description,
        time.ctime(notification["estimated_start_time"]) if "estimated_start_time" in notification else "(N/A)",
        time.ctime(notification["estimated_finish_time"]) if "estimated_finish_time" in notification else "(N/A)",
        notification.get("author", "(N/A)"))
    msg = MIMEText(mail_body, "html", "utf-8")
    msg["Subject"] = mail_subject
    msg["From"] = notification.get("author", "unknown_yt_parrot") + "@yandex-team.ru"
    msg["To"] = ", ".join(recipients)
    logger.debug("Sending the following mail: %s", msg.as_string())
    p = Popen(["/usr/sbin/sendmail", "-t", "-oi"], stdin=PIPE)
    p.communicate(msg.as_string())

def main():
    parser = ArgumentParser()
    parser.add_argument("cluster", type=str, nargs="+")
    parser.add_argument("-r", "--recipient", type=str, nargs="+")
    args = parser.parse_args()
    logger.info("Processing following clusters: %s, sending notifications to following recipients: %s", args.cluster, args.recipient)
    for cluster in args.cluster:
        logger.info("Processing cluster %s", cluster)
        for notification_id in yt.list("//sys/notifications/{0}".format(cluster)): 
            notification = yt.get("//sys/notifications/{0}/{1}".format(cluster, notification_id))
            if notification.get("sent_via_mail", False) or notification.get("hidden", False):
                continue
            logger.info("Sending notification %s", notification_id)
            send_notification(cluster, notification_id, notification, args.recipient)

            yt.set("//sys/notifications/{0}/{1}/sent_via_mail".format(cluster, notification_id), True)
                
if __name__ == "__main__":
    main()
