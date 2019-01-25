# pipechat

Tiny chat program (ab)using UNIX filesystem primitives (and only filesystem primitves):

- directories
- files
- pipes (FIFOs)

This is modifed version of [abenson's](https://github.com/abenson) interesting program, called [`minitalk`](https://github.com/abenson/minitalk). It uses `fifodirs` to sync "clients".

## Inception

"My funnny little story" behind this fork is similar to the one of `abenson`, which they talk about in their own account. Read mine and theirs only if you have time, otherwise see Theory of operation.

I was woking remotely on a Debian server(s), for a client. 

Despite my earnest efforts to move that client away from the platform (there are many funny stories that could be told about those attempts), I ended up stuck with it. 

Why, I thought that move was necessary for that given client, will become apparent later, but it boils down mainly to Debian's lack of novelty (as in "newness").

Main reason for client's Debian lock-in was inertia (as it usually is) and that client had hard requirement on external sysadmins (and these, for some reason, insisted on "Debian only", or so it seemed). Overall it's not terribly important. Debian it is! 

Though they, the client, were also tiny bit "shizophrenic", wanting to use newest technology, on most outdated Debian installs possible. As I was to learn soon, that was match straight from hell. Never again.

This OS, Debian, is known as being sta(b)le for a reason. Yes you get a lot of packages, but you don't really get (any) recent ones. Not even close. 

Trust me, even unimportant manpower starved and dying niche oses like DraghonflyBSD, OpenBSD, FreeBSD or MacOS have more recent packages than Debian. Think about that.

Also packaging bureaucracy of Debian is beyond imaginable. What's worse, these are not just your "regular only slightly/more outdated" packages that upstream produces. These packages are mutilated in specific way. We call this process of package mutilation "a debianization". 

You can know package quite well and even know how to operate it efficiently on 6 different OSes, but not on Debian. There is awlays this little config layout change, this small patch here or there, that doesn't seem very important, but that it really is, in strange sysadmin way. It's for making things "easier for you", you know?

If Kafka saw Debian's organisation, he would defiantely weep tears of joy. It's no wonder veteran Debian admins are so reluctant to use something else. Debianization wears on you.

Anyway, dozens PPA's, few manually built packages and many "Wtf?? it works same way on all BSDs, Linuces, including Ubuntu, and even macOS(!), but not Debian !?"s later, we got the stuff we wanted, running ... somehow.

Paranoic that I am, I always insisted on being present (logged in) during major system upgrades to be ready to rebuild half of the stuff we needed to roll, in event it should break. As chance would have it, more often than not, it happened that upgrade was done in different timeframe, than when I was present, "crossing the fingers" way. Nevermind. It turned out to be not that big of a problem, as stuff seemed to work. Mostly.

Anyway, few rare times, we even managed to sync together (me and other sysadmins). 

While connected, I sometimes checked `who` and saw that other person started working. 

Now, while I was aware that they logged-on, they were not always aware of me. After few strategically sent `wall`'s with carefull use of `touch /tmp/chat` and `chown /tmp/chat`we ended up communicating through `echo 'howdy' >> /tmp/chat`, `cat >> /tmp/chat` and `tail -f /tmp/chat`.

And although after that experience I was tempted to install something "chattable" on Debian several times, seeing dependecies required and/or need for server running for anything, always made me reconsider.

Then, one day, I finally found `minitalk`. What joyful day that was! 

You could just download two raw files from github, read them, `make` it and "**_It just worked!_** tm". It was dead simple and nicely readable. And it worked vanilla, even on Debian!

Using the program more, I quickly became aware of some if it's warts. Main thing, that I wanted from it, was to stop polling the chat log file. I started scratching that itch of mine, and this fork was born.

And, by the way, I guess that by now you already know, that I really love Debian ... not!

## Theory of operation

Instead of periodically polling chat log file in one second long intervals like `minitalk` does, `pipechat` uses `fifodir`. Or rather `chatdir`. But first thing first.

`fifodir` is filesystem based, M:N communication system, discovered by [Mr. Stefan Karrmann](http://www.mathematik.uni-ulm.de/m5/sk/index.html) and polished into beauty and exploited to maximum by [Mr. Laurent "skarnet" Bercot](https://skarnet.org). It is also main workhorse behind notification mechanism of [s6 supervision suite](https://skarnet.org/software/s6/index.html). 

One great thing about `fifodir`s is, that they are dead simple and most importantly, **_serverless_**.

Filesystem directory chosen as `fifodir` is created empty. Now event/notification listeners can "connect" or "register" there, into the `fifodir`, by creating named pipe there each (also known as fifo - made usually by `mkfifo()` system call), and by subsequently `open()`ing their pipe for `read()`ing.

Once open, program can monitor event source fifo by kernel provided eventloop core like `poll()` or `select()`.

When anybody wants to notify all the registered listeners about new event, they do it by `opendir()`ing the `fifodir`, iterate over all the fifos found there, `open()`ing them one by one and by `write()`ing notification message into each one of them.

As listeners are stuck blocked in `select()` or `poll()` (or `epoll()`, or `kevent()` even) on their fifos as readers, they get woken up individually, one after another, by the originator's `write()`s, and by the means of their individual internal `read()`s, event is repeatedly delivered (distributed) to each one of them.

Acces rights are managed by OS kernel itself, so it's as secure as your OS setup is.
To learn more about `fifodir`s visit [s6](https://skarnet.org/software/s6/index.html) and [s6:fifodir](https://skarnet.org/software/s6/fifodir.html).

When invoking `pipechat` you pass it an `chatdir` parameter, like this: `$ pipechat /tmp/chat`.

If `chatdir`, in this example `/tmp/chat`, does not exist, it's created with following structure:

```
/tmp/chat       <- "chatdir" itself - eg pipechat's "chatroom"
├── event       <- "chadir's eventdir", "fifodir" for client event pipes actually
│   └── 1777    <- PID of currently "connected" pipechat instance
└── log         <- chat log, eg. "chatroom"'s contents
```

This is what happens, when you "connect" second `pipechat`instance from different terminal of same user, with same incantation: `$ pipechat /tmp/chat`.

```
/tmp/chat       <- "chatdir" itself - eg "chatroom"
├── event       
│   ├── 1888    <- PID of this pipechat instance
│   └── 1777    <- PID of previous pipechat instance
└── log
```

Now, each time you type-in chat message, instance that you typed it in, will first append it to chat's `log` and only once write succeeds, it will notify other listeners that new message is avalible for read. This way, all the listeners can slumber in "_deep sleep_" until event (new message) happens. 

When terminating `pipechat` instance using `CTRL + D` key chord or by `/quit` command, it's fifo is removed from the `chatdir`'s `eventdir`. This effectively "disconnects" the client.

What happens when all instances quit? 

```
/tmp/chat       <- "chatdir" itself - eg "chatroom"
├── event       <- empty "eventdir"/"fifodir" = no clients
└── log         <- IMPORTANT chat log remains preserved in fs and can still be archived
```

This means that chat logs are not removed (and are thus preserved) unless removed manually with `rm -rf -- /tmp/chat` after all clients have disconnected, or by connecting with  `pipechat`by `$ pipechat /tmp/chat` and by issuing `/destroy` command. 

Always keep that in mind!

Talking to oneself has it's uses (like between two TMUX sessions) but it's much more fun to talk to your buddies. `pipechat` by default sets ownership of the `eventdir` to the user's primary group, which is often not what you want when you want to talk to other people (like when your user is `user` and it's default group is `user`, but all the people you want to talk to, are in group `secretaries`) . 

For multiuser chat communication to work, all users must be in same auxiliary group, and `pipechat` must be instructed to use that group for `eventdir`. 

For example to create chatroom for users in group `users`, you must invoke `pipechat` like this: `$ pipechat /tmp/chat-only-for-users users`.

That incantation will create following setup:

```
/tmp/chat-only-for-users  -> perms:[you:rwx, users:rwx, world:---]
├── event                 -> perms:[you:rwx, users:rwx, world:---]
│   └── 1777              -> perms:[you:rw-, users:-w-, world:---]
└── log                   -> perms:[you:rw-, users:rw-, world:---]
```

Now user `joe` in group `users` can "connect" to `/tmp/chat-only-for-users users`too : `joe$ pipechat /tmp/chat-only-for-users`.

```
/tmp/chat-only-for-users  -> perms:[you:rwx, users:rwx, world:---]
├── event                 -> perms:[you:rwx, users:rwx, world:---]
│   ├── 1888              -> perms:[joe:rw-, users:-w-, world:---]
│   └── 1777              -> perms:[you:rw-, users:-w-, world:---]
└── log                   -> perms:[you:rw-, users:rw-, world:---]
```

Observe, how `fs` permissions allow the whole system to "work" while maintaining relative "security".

## Requirements

- C compiler.
- POSIX system with working `poll()` eventloop core (anything post circa 2001)
- GNU readline

## Usage

    $ pipechat path/to/chatdir

It uses your username as the nick. Each line is prefixed by pipechat isntance PID for multi client identification. You can specify user group through second parameter:

    $ pipechat path/to/chatdir sysops

To send a message, type it and hit enter. 

To quit, type `/quit`.

To destroy chatroom (`chatdir`) type `/destroy`. This will "autoquit" all other "clients" too, and pipechat will destroy `chatdir` (including chat log).

To learn other supported commands use `/help` command.

## Security disclaimer

The primary issue here is the same as with `minitalk`: a security concern. 

Anyone that is chatting in the "chatroom" has to be able to read from, and write to, the `log` file and has to be at least able to write into each `fifodir`'s pipe (to be able to send notifications to other clients).

Anyone that has access to these "control" files can also inject anything they want into the chatroom and the chat log.

More over, unless explicitly destroyed, log persists even after all chat user have "disconnected".

As such, it is highly advisory to create chatrooms only in RAM only filesystems (like `tmpfs`), and destroy them explicitly after use.

This is truly built on top of the "honor" and `fs` permissions system, and was designed around a single purpose: efficient multi-user chat between trusted users within single, trusted, host.

Don't expect this to be secure.

![A demo!](docs/pipechat.gif)
