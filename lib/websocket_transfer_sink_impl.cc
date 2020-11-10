/* -*- c++ -*- */
/*
 * Copyright 2020 Pavel Fiala, UwB, 2020.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// Gnuradio includes ...
// ---------------------
#include <gnuradio/io_signature.h>
#include <gnuradio/blocks/pdu.h>
#include <gnuradio/thread/thread.h>
#include <gnuradio/digital/crc32.h>
#include <gnuradio/logger.h>

// Boost includes ...
// ------------------
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/http/message.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/beast/websocket/stream.hpp>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>

// System includes ...
// -------------------
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <regex>

// Gnuradio modules include ...
// ----------------------------
#include "websocket_transfer_sink_impl.h"

// Define constants ...
// --------------------
#define CONST_WRITE_BUFFER_SIZE 10
#define CONST_SLEEP_INTERVAL_MILI_SECONDS 100
 
// #define CONST_DEBUG
#define CONST_DEBUG_WEBSOCKET

namespace gr {
  namespace gsSDR {
      
    class set_subprotocols{
        std::string s_;
    public:
        explicit
        set_subprotocols(std::string s)
            : s_(s) {}

        template<bool isRequest, class Body, class Headers>
        void
        operator()(boost::beast::http::message<isRequest, Body, Headers>& m) const
        {
            m.set("X-Custome-Id", s_);
        }
    };
  
    // Short alias for this namespace ...
    // ----------------------------------
    namespace beast = boost::beast;         // from <boost/beast.hpp>
    namespace http = beast::http;           // from <boost/beast/http.hpp>
    namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
    namespace net = boost::asio;            // from <boost/asio.hpp>
    using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

    namespace pt = boost::property_tree;
    using namespace boost::posix_time;  
      
    using namespace std;
    
    // Define static variables ...
    // ---------------------------
    fifo_buffer websocket_transfer_sink_impl::m_fifo;
    bool websocket_transfer_sink_impl::m_exit_requested;


    websocket_transfer_sink::sptr
    websocket_transfer_sink::make(const std::string &ServerName, const std::string &ServerPort, int stationId, const std::string &UserName, const std::string &UserPass)
    {
      return gnuradio::get_initial_sptr
        (new websocket_transfer_sink_impl(ServerName, ServerPort, stationId, UserName, UserPass));
    }

    /*
     * The private constructor ...
     * ---------------------------
     */
    websocket_transfer_sink_impl::websocket_transfer_sink_impl(const std::string &ServerName, const std::string &ServerPort, int stationId, const std::string &UserName, const std::string &UserPass)
      : gr::block("websocket_transfer_sink",
              gr::io_signature::make(0, 0, 0),  //  -- gr::io_signature::make(<+MIN_IN+>, <+MAX_IN+>, sizeof(<+ITYPE+>)),
              gr::io_signature::make(0, 0, 0)), //  -- gr::io_signature::make(<+MIN_OUT+>, <+MAX_OUT+>, sizeof(<+OTYPE+>)))
              m_ServerName(ServerName), m_ServerPort(ServerPort), m_stationId(stationId), m_UserName(UserName), m_UserPass(UserPass)
    {
       
       m_packetCounter = 0; m_exit_requested = false; 
       m_fifo.fifo_changeSize(32);

       // Register PDU input port ...
       // --------------------------- 
       message_port_register_in(pmt::mp("in"));
       set_msg_handler(pmt::mp("in"), boost::bind(&websocket_transfer_sink_impl::packet_handler, this, _1));
       
       /* Create read stream THREAD [http://antonym.org/2009/05/threading-with-boost---part-i-creating-threads.html]
          and [http://antonym.org/2010/01/threading-with-boost---part-ii-threading-challenges.html]
       */

       
#ifdef CONST_DEBUG       
      cout << "Threading - using up to CPUs/Cores: "<< boost::thread::hardware_concurrency() <<endl;
#endif
      _thread = gr::thread::thread(&websocket_transfer_sink_impl::websocket_transfer_sink_wait,this); 
        
    }

    /*
     * Our virtual destructor ...
     * --------------------------
     */
    websocket_transfer_sink_impl::~websocket_transfer_sink_impl(){
        
      // Only for debug  ...
      // -------------------
#ifdef CONST_DEBUG          
      cout<< "Calling destructor - websocket_transfer_sink_impl ... "<< endl;
#endif
      // De-init msgdma --> close Linux device driver ...
      // ------------------------------------------------
      threadTransferDeInit();

#ifdef CONST_DEBUG
      cout<< "Calling destructor - websocket_transfer_sink_impl - EnDL... "<< endl;
#endif        
        
    }
    
    /*
    bool websocket_transfer_sink_impl::stop(){
        
        // Only for debug  ...
        // -------------------
        cout<< "Calling STOP - websocket_transfer_sink_impl ... "<< endl;
        
        // De-init msgdma --> close Linux device driver ...
        // ------------------------------------------------
        threadTransferDeInit();
      
        return true;
    }
    */
    
    // msgdmaDeInit function [private] ...
    // -----------------------------------
    void websocket_transfer_sink_impl::threadTransferDeInit(){
        
        gr::thread::scoped_lock lock(fp_mutex);   // shared resources ...
        m_exit_requested = true;
        lock.unlock();

        if(_thread.joinable()) _thread.join();
     
    }
    

    void websocket_transfer_sink_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required){
      /* <+forecast+> e.g. ninput_items_required[0] = noutput_items */
    
    }

    int websocket_transfer_sink_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    { 
        
      /*  
      const <+ITYPE+> *in = (const <+ITYPE+> *) input_items[0];
      <+OTYPE+> *out = (<+OTYPE+> *) output_items[0];
      */
      
      // Do <+signal processing+>
      // ------------------------
      // Tell runtime system how many input items we consumed on
      // each input stream.
      consume_each (noutput_items);

      // Tell runtime system how many output items we produced.
      return noutput_items;
    }
    
    // altera_msgdma_sdr_wait function [provides new THREAD] - [private /* static */] ...
    // ----------------------------------------------------------------------------------
    void websocket_transfer_sink_impl::websocket_transfer_sink_wait(){
        
        int niutput_items_real = 0;
        
        gr_storage_web buffer_write_storage[CONST_WRITE_BUFFER_SIZE];

#ifdef CONST_DEBUG
        cout << "websocket_transfer_sink_wait SDR processing THREAD was started ..." << endl;
#endif
        if(m_exit_requested){
#ifdef CONST_DEBUG            
           cout<< "websocket_transfer_sink_wait processing THREAD ends - clearing resources ... "<< endl;   
#endif
           return; 
        }
        
        // While TRUE loop ...
        // -------------------
        while(true){
            // Read from internal circular FIFO buffer ...
            // -------------------------------------------
            niutput_items_real = m_fifo.fifo_read_storage(buffer_write_storage, CONST_WRITE_BUFFER_SIZE);
            
            if(niutput_items_real > 0){
                
               // Send JSON data ...
               // ------------------ 
               for(int i=0;i<niutput_items_real;i++){
                   
                    try{
                        // The io_context is required for all I/O ...
                        // ------------------------------------------
                        net::io_context ioc;

                        // These objects perform our I/O ...
                        // ---------------------------------
                        tcp::resolver resolver{ioc};
                        websocket::stream<tcp::socket> ws{ioc};

                        // Look up the domain name ...
                        // ---------------------------
                        auto const results = resolver.resolve(m_ServerName.c_str(), m_ServerPort.c_str());

                        // Make the connection on the IP address we get from a lookup ...
                        // --------------------------------------------------------------
                        net::connect(ws.next_layer(), results.begin(), results.end());

                        // Perform the websocket handshake ...
                        // -----------------------------------
                        ws.handshake_ex(m_ServerName.c_str(), "/", set_subprotocols{"echo-protocol"});

                        size_t wr_size = ws.write(net::buffer(buffer_write_storage[i].getString()));
#ifdef CONST_DEBUG             
                        std::cout<<"WebSocket write size: "<<wr_size<<std::endl;
#endif
                        // This buffer will hold the incoming message ...
                        // ----------------------------------------------
                        beast::flat_buffer buffer;

                        // Read a message into our buffer ...
                        // ----------------------------------
                        size_t rd_size = ws.read(buffer);
#ifdef CONST_DEBUG             
                        std::cout<<"WebSocket read size: "<<wr_size<<std::endl;
#endif            
                        
                        // Close the WebSocket connection ...
                        // ----------------------------------
                        ws.close(websocket::close_code::normal);

                        // If we get here then the connection is closed gracefully ...
#ifdef CONST_DEBUG_WEBSOCKET     
                        // The make_printable() function helps print a ConstBufferSequence
                        std::cout << beast::buffers_to_string(buffer.data()) << std::endl;
#endif            
                        
                    }

                    catch(std::exception const& e){
                        std::cerr << "Error: " << e.what() << std::endl;
                        // return EXIT_FAILURE;
                    }
                   
               }
            }
            
            // Waiting for data ...
            // --------------------
            boost::this_thread::sleep(boost::posix_time::milliseconds(CONST_SLEEP_INTERVAL_MILI_SECONDS));
            
            gr::thread::scoped_lock lock(fp_mutex);   // shared resources ...
            if(m_exit_requested) break;               // shared global variable - exit requested if m_exit_requested == TRUE --> break ...  
        }
    }
    
    // packet_handler - public function / callback ...
    // -----------------------------------------------
    void websocket_transfer_sink_impl::packet_handler(pmt::pmt_t msg){
        
        int packetSize = 0;
        
        // Extract payload ...
        // -------------------
        vector<unsigned char> data_uint8(pmt::u8vector_elements(pmt::cdr(msg)));
        packetSize = data_uint8.size();
        
#ifdef CONST_DEBUG           
           cout<<"Packet size: "<<packetSize<<endl;
#endif             
        
        // Extract meta information /verbose ...
        // -------------------------------------
        pmt::pmt_t meta = pmt::car(msg);
        
        if(pmt::dict_has_key(meta, pmt::string_to_symbol("syncword"))) {
           pmt::pmt_t r = pmt::dict_ref(meta, pmt::string_to_symbol("syncword"), pmt::PMT_NIL); 
#ifdef CONST_DEBUG           
           cout<<"Syncword: "<<pmt::to_long(r)<<endl;
#endif           
        }
        
        // Create a ptree (json) root ...
        // ------------------------------
        pt::ptree oroot;
        
        // Create JSON ...
        // ---------------
        ptime t = microsec_clock::universal_time();
        
#ifdef CONST_DEBUG           
        std::cout <<"Date Time: "<< to_iso_extended_string(t) << "Z"<<std::endl; // "\n"
#endif          
        
        oroot.put("ground_station", m_stationId);
        oroot.put("dateTime", to_iso_extended_string(t)+"Z");
        oroot.put("crc_ok", true);
        oroot.put("packet_number",m_packetCounter++);
        oroot.put("packet_length", packetSize);
        
        // Add a list / matrix ...
        // -----------------------
        pt::ptree matrix_node;
        
        for (int i=0;i<packetSize;i++){
            // Create an unnamed node containing the value ...
            // -----------------------------------------------
            pt::ptree node;
        
            node.put("", (boost::format("0x%02X") % (int)data_uint8.at(i)).str()); // %02X -- without 0x

            // Add this node to the list ...
            // -----------------------------
            matrix_node.push_back(std::make_pair("", node));
        }

        oroot.add_child("packet_array", matrix_node);
        
        std::ostringstream buf;
        write_json (buf, oroot, false);
        std::string json = buf.str();

        std::regex reg("\\\"((?:0[xX])?[0-9a-fA-F]+)\\\"");                 // std::regex reg("\\\"([0-9]+\\.{0,1}[0-9]*)\\\"");
        std::string json_corection_1 = std::regex_replace(json, reg, "$1");
        std::regex reg_2("\\\"(true|false)\\\"");
        std::string json_corection_2 = std::regex_replace(json_corection_1, reg_2, "$1");

#ifdef CONST_DEBUG         
        cout <<json_corection_2<<endl;
#endif
        // Add to circular buffer ...
        // --------------------------
        gr_storage_web storage = gr_storage_web(0,json_corection_2);
        m_fifo.fifo_write_storage(&storage, 1);
        
    }

  } /* namespace gsSDR */
} /* namespace gr */

