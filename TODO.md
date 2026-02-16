for output onodes:

- [x] create an input stream to take audio data
- [x] copy audio data into the coressponding vnode
- [x] ensure vnode is connected to the same output device as the onode, either one way sync or double sync (not recommended)
- [x] lock chromium volume to 100%

for input onodes:

- [ ] create a virtual mic
- [ ] onode must be using that virtual mic, and then the vnode will be connected to the vmic's original
- [ ] audio data is copied from vnode into that virtual mic
- [ ] vnode must be connected to the correct microphone (complications)
- [ ] vmic volume should be locked to 100%

- the most user-friendly way is to make a virtual mic for each connected/activated mic on the system, sth like [system mic name] (pipetron)
- user can choose the electron app to connect to the pipetron version of the mic, and the vnode will capture the audio from the correct system mic and copy it to that vmic

other:

- [ ] config file to determine daemon mode to start
- [ ] work on fixmes and todos in code
