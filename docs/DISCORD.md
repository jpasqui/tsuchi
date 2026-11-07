# Discord announcements

Publishing a GitHub release sends its name, link, and release notes to Discord.
Published prereleases use the same announcement. Drafts and CI builds send no message.

The workflow uses the existing [Discord Webhook Action](https://github.com/tsickert/discord-webhook),
pinned to a reviewed commit. Until a webhook is configured, the workflow logs a notice and
skips delivery.

## Configure Discord

1. Create the channel that will receive releases. In the server's webhook settings, create
   a webhook for that channel and copy its URL. Discord's [webhook guide](https://support.discord.com/hc/en-us/articles/228383668-Intro-to-Webhooks)
   explains the server permissions and setup.
2. In this repository's **Settings > Secrets and variables > Actions**, add a repository
   secret named `DISCORD_WEBHOOK_URL`. Store the copied webhook URL as its value.
3. To send into an existing thread, create the thread first and add an Actions repository
   variable named `DISCORD_THREAD_ID` with its numeric ID. Keep the webhook URL free of query
   parameters. The webhook's channel must contain that thread. Without this variable, messages
   go to the webhook's channel. A forum channel needs an existing thread ID.

Keep the URL in the secret store. Anyone with the URL can post through the webhook.
There is no bot to install or service to host.

## Publish notes

Review the draft created by the release workflow, write the patch notes in the release's
description, and choose **Publish release** on GitHub. The announcement uses that description
as its notes, so the release page and Discord share one source.

Discord embeds have a description limit. The action sends the first 4,096 characters of
long notes, and the linked release title opens the full notes and downloads on GitHub.
The message uses an embed, with no separate message text or configured mention.

The notification workflow runs on GitHub's [`release.published` event](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release).
If automatic publishing is added later, account for GitHub's [workflow token event rules](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow#triggering-a-workflow-from-a-workflow).
A release published by `GITHUB_TOKEN` does not start another workflow. Publishing the reviewed
draft in GitHub's interface triggers the announcement.

## Check delivery

After publishing, open the **Discord release announcement** workflow in Actions. A configured
webhook waits for Discord to confirm delivery. A delivery error fails the notification job
so it can be inspected and rerun. Rerunning a successful notification sends another message.

The webhook and thread can be configured later. No live test message is sent during repository
setup.
