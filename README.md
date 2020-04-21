# pipechat

Tiny chat program (ab)using UNIX filesystem primitives (and only filesystem primitves):

- directories
- files
- pipes (FIFOs)

This is modifed version of [abenson's](https://github.com/abenson) interesting program, called [`minitalk`](https://github.com/abenson/minitalk). It uses `fifodirs` to sync "clients".

## Inception

"My funnny little story" behind this fork is similar to the one of `abenson`, which they talk about in their own account. Read mine, and theirs, below. But only if you have time, otherwise see Theory of operation.

I was woking remotely on a Debian server, for a client. 

Despite my earnest efforts to move that client off that platform, Debian (there are many funny stories that could be told about those attempts), I ended up being stuck with it. 

Why I thought that move was necessary for that given client, will become apparent later, but it boils down mainly to Debian's lack of "novelty" (as in "newness").

Main reason for client's Debian lock-in was inertia (as it often is) and also the fact that that client had hard requirement on external sysadmins (and these, for some reason, insisted on "Debian only or no go"). Overall it's not terribly important. Debian it was! 

Such arrangement would not seem so unusual, but they, the client, were also tiny bit "shizophrenic". They wanted to use newest technology, on most outdated Debian installs possible. As I was to learn soon, that was match straight from hell. Never again.

This OS, Debian, is known as being sta(b)le for a reason. Yes you get a lot of packages, and you get exceptional stability (or rather staleness), but you don't really get any, even remotely, recent software. Not even close. 

Trust me, even "unimportant" manpower starved and "dying" "niche" oses like DraghonflyBSD, OpenBSD, FreeBSD or MacOS have more recent packages than Debian. Think about that.

Also packaging bureaucracy of Debian is beyond imaginable. What's worse, these are not just your "regular but slightly/more outdated" packages that package upstream produces. These packages are mutilated in specific, Debian only, way. We call this process of package mutilation "a debianization". 

You see, you can "know" software package quite well and even know how to operate it efficiently on 6 different OSes, but not on Debian. On Debian this package will look nothing like itself. There is awlays this little config layout change, this small patch here or there, that doesn't seem to be very important, but that really is, especially in strange sysadmin related way. They do it to make things "easier for you", you know?

If Kafka saw Debian's organisation, he would definately weep tears of joy. It's no wonder veteran Debian admins are so reluctant to use something else. Debianization wears on you. One day you give in and next day you wake up completely debinanized.

Anyway, dozens PPA's, few manually built packages and many "Wtf?? it works same way on all BSDs, Linuces, including Ubuntu, and even macOS(!), but not Debian !?"'s later, we got the stuff we wanted, running ... somehow. I almost became debianized myself in the process.

But back to the story. Paranoic that I am, I always insisted on being present (logged in) during major system upgrades to be ready to rebuild half of the stuff we needed to roll, in event it should break (because the amount of custom packages). As chance would have it, more often than not, it happened that upgrade was done by external admins in different timeframe, than when I was available, "crossing the fingers" way. Nevermind. It turned out to be not that big of a problem, as stuff seemed to work. Mostly.

Anyway, few rare times, we even managed to sync together (me and other sysadmins). 

While connected, I sometimes checked `who` and saw that other person started working. 

Now, while I was aware that they logged-on, they were not always aware of me. After few strategically sent `wall`'s with carefull use of `touch /tmp/chat` and `chown /tmp/chat`we ended up communicating through `echo 'howdy' >> /tmp/chat`, `cat >> /tmp/chat` and `tail -f /tmp/chat`.

And although after that experience I was tempted to install something "chattable" on Debian several times, like ircd. But seeing dependecies required and/or need for server running for anything (irc), always made me reconsider.

Then, one day, I stumbled upon `minitalk`. What joyful day that was! 

You could just download two raw files from github, read them, `make` it and "**_It just worked!_** tm". Code was dead simple and nicely readable. And it worked vanilla, even on Debian! No deps!

Using the program more, I quickly became aware of some of it's shortcomings. Main thing, that I wanted from it, was to stop polling the chat log file. In it's original form, it polls "chat file" every second or so. I started scratching that itch of mine, and that's how this fork was born.

So there you have it, nice little story to read before going to bed. And, by the way, I guess that by now you already know, that I really love Debian ... not!

## Theory of operation

Instead of periodically polling chat log file in one second long intervals like `minitalk` does, `pipechat` uses `fifodir`. Or rather `chatdir`. 

But first thing first.

`fifodir` is filesystem based, M:N communication system, discovered by [Mr. Stefan Karrmann](http://www.mathematik.uni-ulm.de/m5/sk/index.html) and polished into beauty (and also exploited to maximum) by [Mr. Laurent "skarnet" Bercot](https://skarnet.org). It is also main workhorse behind notification mechanism of [s6 supervision suite](https://skarnet.org/software/s6/index.html). 

One great thing about `fifodir`s is, that they are dead simple and most importantly, **_serverless_**.

Filesystem directory chosen as `fifodir` is created empty. Now event/notification listeners can "connect" or "register" there( "into" the `fifodir`) by creating named pipe there each (pipe also known as fifo - made usually by `mkfifo()` system call). We will call this procedure a "registration". By subsequently `open()`ing this pipe of theirs for `read()`ing, interested processes can enter the next phase. 

Once open, each process can treat their fifo as notification/event source by monitoring it with kernel provided eventloop core like `poll()` or `select()`. This activity can be described as "monitoring".

When anybody wants to notify all the other "registered listeners" about a new event, they do it by `opendir()`ing the `fifodir`, iterate over all the fifos found there, `open()`ing them one by one and by `write()`ing notification message into each one of them. This is bascially a notification broadcast.

As listeners are blocked (or waiting) in `select()`, or `poll()` (or `epoll()`, or `kevent()` even) on their input fifos as readers, they get woken up individually, one after another, each by the event originator's `write()`s. Then by the means of their individual internal `read()`s, given event is repeatedly delivered (distributed) to each one of them.

Access rights are managed by OS kernel itself, so it's as secure as your OS setup is. So much for the main indea of operation.

To learn more about `fifodir`s visit [s6](https://skarnet.org/software/s6/index.html) and [s6:fifodir](https://skarnet.org/software/s6/fifodir.html). It is very interesting system.

When invoking `pipechat` you pass it an `chatdir` parameter, like this: `$ pipechat /tmp/chat`.

If `chatdir`, in this example `/tmp/chat`, does not exist, it's created with following structure:

```
/tmp/chat       <- "chatdir" itself - eg pipechat's "chatroom"
├── event       <- "chadir's eventdir", eg "fifodir" for client event pipes
│   └── 1777    <- PID of currently "connected" pipechat instance
└── log         <- chat log, eg. actual "chatroom"'s contents
```

This is what happens, when you "connect" second `pipechat`instance from different terminal of same user, with same incantation: `$ pipechat /tmp/chat`.

```
/tmp/chat       <- "chatdir" itself - eg "chatroom"
├── event       
│   ├── 1888    <- PID of this pipechat instance
│   └── 1777    <- PID of previous pipechat instance
└── log
```

Now, each time you type-in chat message, instance that you typed it in, will first append it to chat's `log` and only once that write succeeds, it will notify other listeners that new message is avalible for read. This way, all the listeners can slumber in "_deep sleep_" just until event happens (new chat message arrives).

When terminating `pipechat` instance using `CTRL + D` key chord, or by `/quit` command, it's fifo is removed from the `chatdir`'s `eventdir`. That effectively "disconnects"/"unregisters" that client.

What happens when all instances quit? 

```
/tmp/chat       <- "chatdir" itself - eg "chatroom"
├── event       <- empty "eventdir"/"fifodir" = no clients
└── log         <- IMPORTANT chat log remains preserved in fs and can still be archived (or stolen!)
```

This means that chat logs are not removed (and are thus preserved) after all the clients have disconnected, unless removed manually with `rm -rf -- /tmp/chat` (or by connecting with  `pipechat`by `$ pipechat /tmp/chat` and by issuing `/destroy` command). 

Always keep that in mind! Always destroy logs on the last disconnection (or with `rm`) if you don't want to keep them.

Talking to oneself has it's uses (like chatting with oneself between two TMUX sessions) but it's much more fun to talk to your buddies! 

`pipechat` by default sets ownership of the `eventdir` to the user's primary group, which is often not what you want when you want to talk to other people (like when your user is `user` and it's default group is `user`, but all the people you want to talk to, are in group `secretaries`). 

For multiuser chat communication to work, all users must be in same auxiliary group, and `pipechat` must be instructed to apply that group to `eventdir`. 

For example to create chatroom for users in group `users`, you must invoke `pipechat` like this: `$ pipechat /tmp/chat-only-for-users users`.

That incantation will create following setup:

```
/tmp/chat-only-for-users  -> perms:[you:rwx, users:rwx, world:---]
├── event                 -> perms:[you:rwx, users:rwx, world:---]
│   └── 1777              -> perms:[you:rw-, users:-w-, world:---]
└── log                   -> perms:[you:rw-, users:rw-, world:---]
```

Only now user `joe` in group `users` can "connect" to `/tmp/chat-only-for-users users`! Like this : `joe$ pipechat /tmp/chat-only-for-users`.

```
/tmp/chat-only-for-users  -> perms:[you:rwx, users:rwx, world:---]
├── event                 -> perms:[you:rwx, users:rwx, world:---]
│   ├── 1888              -> perms:[joe:rw-, users:-w-, world:---]
│   └── 1777              -> perms:[you:rw-, users:-w-, world:---]
└── log                   -> perms:[you:rw-, users:rw-, world:---]
```

Observe, how `fs` permissions allow the whole system to "work" while still maintaining relative "security".

## Requirements

- C compiler.
- POSIX system with working `poll()` eventloop core (anything post circa 2001)
- GNU readline

## Usage

You start chat in chatdir `path/to/chatdir` like this:

    $ pipechat path/to/chatdir

Pipechat will use your username as the nick. Each line in chatlog is accompanied by pipechat instance PID for multi client identification. 

If you want multiuser chat between users of specific group, you can specify that user group through second parameter:

    $ pipechat path/to/chatdir sysops
    
Keep in mind you actually **have to be member of that group** to have permissions to do so!

To send a message, type it into the pipechat's command line and hit enter. 

To quit, type `/quit`.

To destroy chatroom (eg `chatdir`) type `/destroy`. This will "autoquit" all other "clients" too, and pipechat will destroy `chatdir` (including chat log) with `remove()` syscall.

To learn other supported commands use builtin `/help` command.

## Security disclaimer

The primary issue here is the same as with `minitalk`: a security concern. 

Anyone that is chatting in the "chatroom" has to be able to read from, and write to, the `log` file and has to be at least able to write into each `fifodir`'s pipe (to be able to send notifications to other "clients").

Anyone that has access to these "control" files can also inject anything they want into the chatroom and the chat log.

More over, unless explicitly destroyed, log persists even after all chat user have "disconnected".

As such, it is highly advisory to create chatrooms only in RAM-only filesystems (like `tmpfs`), and destroy them explicitly after use.

This is truly built on top of the "honor" and `fs` permissions system, and was designed around a single purpose: efficient multi-user chat between trusted users within single, trusted, host/node. These design constraints allow the system to remain "serveless" and just work.

Don't expect this to be secure. If you are truly paranoid you can use SELinux to lock down the access further.

![A demo!](docs/pipechat.gif)
