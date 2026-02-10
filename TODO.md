for output onodes:

- [ ] create an input stream to take audio data
- [ ] copy audio data into the coressponding vnode
- [ ] ensure vnode is connected to the same output device as the onode, either one way sync or double sync (not recommended)

for input onodes:

- [ ] create a virtual mic
- [ ] onode must be using that virtual mic
- [ ] audio data is copied from vnode into that virtual mic
- [ ] vnode must be connected to the correct microphone (complications)

- the most user-friendly way is to make a virtual mic for each connected/activated mic on the system, sth like [system mic name] (pipetron)
- user can choose the electron app to connect to the pipetron version of the mic, and the vnode will capture the audio from the correct system mic and copy it to that vmic

other:

- [ ] config file to determine daemon mode to start
