from tapdevice import TapDevice
from gnuradio import blocks
from gnuradio import channels
from gnuradio import digital
from gnuradio import eng_notation
from gnuradio import fec
from gnuradio import gr
from gnuradio import audio
from gnuradio import analog
from gnuradio import filter
from gnuradio.eng_option import eng_option
from gnuradio.filter import firdes
from optparse import OptionParser
from tapdevice import TapDevice
import numpy
import sys
import threading
import struct

class packet_source(gr.sync_block):
    def __init__(self):
        gr.sync_block.__init__(self,
                name="packet_source",
                in_sig=[],
                out_sig=[numpy.int8])
        self.packet_data = None
        self.ix = 0

    def work(self, input_items, output_items):
        if self.ix < len(self.packet_data):
            data_slice = self.packet_data[self.ix:self.ix+len(output_items[0])]
            data_bin = numpy.fromstring(data_slice, numpy.int8)
            output_items[0][:len(data_bin)] = data_bin
            transmitted_bytes = min(len(self.packet_data)-self.ix, len(output_items[0]))
            self.ix += transmitted_bytes
        else:
            return -1
        return transmitted_bytes

    def add_packet(self, data):
        #TODO Add proper preamble, header, ECC
        self.packet_data = struct.pack('<III', 0, 0, len(data)) + data + struct.pack('<II', 0, 0)
        self.ix = 0


class tap_sink(gr.sync_block):
    def __init__(self, dev):
        gr.sync_block.__init__(self,
                name="tap_sink",
                in_sig=[numpy.int8],
                out_sig=[])
        self.dev = dev

    def work(self, input_items, output_items):
        #TODO detect header, parse packet length from header, parse packet
        #TODO check ECC
        #TODO write packet to tapdev
        return len(input_items[0])


class mod_block(gr.top_block):
    def __init__(self, samp_rate = 44100, carrier_freq = 1000):
        gr.top_block.__init__(self, 'mod_top_block')
        ##################################################
        # Variables
        ##################################################
        self.samp_rate = samp_rate
        self.carrier_freq = carrier_freq

        ##################################################
        # Blocks
        ##################################################
        #TODO work out the bandwidth used, we might be getting aliases now.
        self.digital_psk_mod_0 = digital.psk.psk_mod(
          constellation_points=2,
          mod_code="gray",
          differential=True,
          samples_per_symbol=2,
          excess_bw=0.35,
          verbose=False,
          log=False,
          )

        self.packet_source = packet_source()
        self.blocks_multiply_xx_0 = blocks.multiply_vcc(1)
        self.blocks_complex_to_float_0 = blocks.complex_to_float(1)
        self.blocks_add_xx_0 = blocks.add_vff(1)
        self.audio_sink_0 = audio.sink(samp_rate, "", True)
        self.analog_sig_source_x_0 = analog.sig_source_c(samp_rate, analog.GR_COS_WAVE, carrier_freq, 1, 0)

        ##################################################
        # Connections
        ##################################################
        self.connect((self.analog_sig_source_x_0, 0), (self.blocks_multiply_xx_0, 0))    
        self.connect((self.blocks_add_xx_0, 0), (self.audio_sink_0, 0))    
        self.connect((self.blocks_complex_to_float_0, 0), (self.blocks_add_xx_0, 0))    
        self.connect((self.blocks_complex_to_float_0, 1), (self.blocks_add_xx_0, 1))    
        self.connect((self.blocks_multiply_xx_0, 0), (self.blocks_complex_to_float_0, 0))    
        self.connect((self.packet_source, 0), (self.digital_psk_mod_0, 0))    
        self.connect((self.digital_psk_mod_0, 0), (self.blocks_multiply_xx_0, 1))    

    def add_packet(self, data):
        self.packet_source.add_packet(data)


class demod_block(gr.top_block):
    def __init__(self, tapdev):
        gr.top_block.__init__(self, 'demod_top_block')
        ##################################################
        # Variables
        ##################################################
        self.transition_bw = transition_bw = 500
        self.samp_rate = samp_rate = 44100
        self.center_freq = center_freq = 3000
        self.bandwidth = bandwidth = 4000

        ##################################################
        # Blocks
        ##################################################
        self.freq_xlating_fir_filter_xxx_0 = filter.freq_xlating_fir_filter_ccc(1, (firdes.low_pass(1,samp_rate, bandwidth, transition_bw)), center_freq, samp_rate)
        self.digital_psk_demod_0 = digital.psk.psk_demod(
          constellation_points=8,
          differential=True,
          samples_per_symbol=2,
          excess_bw=0.35,
          phase_bw=6.28/100.0,
          timing_bw=6.28/100.0,
          mod_code="gray",
          verbose=False,
          log=False,
          )
        self.tap_sink = tap_sink(tapdev)
        self.blocks_float_to_complex_0 = blocks.float_to_complex(1)
        self.audio_source_0 = audio.source(samp_rate, "", True)
        self.analog_const_source_x_0 = analog.sig_source_f(0, analog.GR_CONST_WAVE, 0, 0, 0)

        ##################################################
        # Connections
        ##################################################
        self.connect((self.analog_const_source_x_0, 0), (self.blocks_float_to_complex_0, 1))    
        self.connect((self.audio_source_0, 0), (self.blocks_float_to_complex_0, 0))    
        self.connect((self.blocks_float_to_complex_0, 0), (self.freq_xlating_fir_filter_xxx_0, 0))    
        self.connect((self.digital_psk_demod_0, 0), (self.tap_sink, 0))    
        self.connect((self.freq_xlating_fir_filter_xxx_0, 0), (self.digital_psk_demod_0, 0))

def demod_thread(tapdev):
    demod = demod_block(tapdev)
    demod.run()

def main():
    tapdev = TapDevice()
    mod = mod_block()
    thr = threading.Thread(target=demod_thread, args=[tapdev])
    #TODO solve shutting down properly
    #currently the demod thread gets stuck, and the program does not terminate
    #if we set the background thread to daemon, the tap interface is not released properly
    #thr.daemon = True
    thr.start()
    while True:
        data = tapdev.read()
        print('got packet(%s)'%len(data))
        mod.add_packet(data)
        print('running mod')
        mod.run()
        print('mod done')

if __name__=='__main__':
    main()
