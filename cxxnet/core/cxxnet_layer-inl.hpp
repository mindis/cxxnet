#ifndef CXXNET_LAYER_INL_HPP
#define CXXNET_LAYER_INL_HPP
#pragma once
/*!
 * \file cxxnet_layer-inl.hpp
 * \brief implementation of different layers
 * \author Bing Xu, Tianqi Chen
 */

#include "cxxnet_core.h"
#include "cxxnet_op.h"
#include "mshadow/tensor_container.h"

#if CXXNET_ADAPT_CAFFE
#include "cxxnet_caffe_adapter-inl.hpp"
#endif

namespace cxxnet{
    template<typename xpu>
    inline ILayer* CreateLayer_( int type, mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out );

};

namespace cxxnet{
    // expr is needed to use expression
    using namespace mshadow::expr;
    using namespace mshadow::utils;
    // Random init method
    using namespace cxxnet::rnd_type;
    /*! \brief potential parameters for each layer */
    struct LayerParam{
        /*! \brief number of hidden layers */
        int num_hidden;
        /*! \brief initialization sd for weight */
        float init_sigma;
        /*! \brief initialization random type */
        int random_type;
        /*! \brief number of output channel */
        int num_channel;
        /*! \brief kernel size */
        int kernel_size;
        /*! \brief stride prameter */
        int stride;
        /*! \brief whether not include bias term */
        int no_bias;
        /*! \brief dropout threshold  */
        float dropout_threshold;
        LayerParam( void ){
            init_sigma = 0.01f;
            num_hidden = 0;
            random_type = 0;
            num_channel = 0;
            kernel_size = 0;
            stride = 1;
            dropout_threshold = 0;
            no_bias = 0;
        }
        /*!
         * \brief Set param for the layer from string
         * \param name parameter name
         * \param val string for configuration
         */
        inline void SetParam(const char *name, const char* val) {
            if( !strcmp( name, "init_sigma") )  init_sigma = (float)atof(val);
            if( !strcmp( name, "nhidden") )     num_hidden = atoi(val);
            if( !strcmp( name, "random_type"))  random_type = atoi(val);
            if( !strcmp( name, "nchannel") )    num_channel = atoi(val);
            if( !strcmp( name, "kernel_size") ) kernel_size = atoi(val);
            if( !strcmp( name, "stride") )      stride      = atoi(val);
            if( !strcmp( name, "no_bias") )     no_bias = atoi(val);
            if( !strcmp( name, "threshold"))    dropout_threshold = (float)atof(val);
        }
    };
};

namespace cxxnet {
    // simple fully connected layer that connects two nodes
    template<typename xpu>
    class FullConnectLayer : public ILayer{
    public:
        FullConnectLayer( mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out )
            :rnd_(rnd), in_(in), out_(out) {}
        virtual ~FullConnectLayer( void ){}
        virtual void Forward(bool is_train) {
            index_t nbatch = in_.data.shape[1];
            out_.mat()  = dot( in_.mat(), wmat_ );
            if( param_.no_bias != 0 ){
                out_.mat() += repmat( bias_, nbatch );
            }
        }
        virtual void Backprop(bool prop_grad){
            index_t nbatch = in_.data.shape[1];
            real_t scale = 1.0f / nbatch;
            gwmat_ = scale * dot( in_.mat().T(), out_.mat() );
            if( param_.no_bias != 0 ){
                gbias_ = scale * sum_rows( out_.mat() );
            }
            // backprop
            if( prop_grad ){
                in_.mat() = dot( out_.mat(), wmat_.T() );
            }
        }
        virtual void AdjustNodeShape( void ) {
            Assert( in_.is_mat(), "input need to be a matrix" );
            Assert( param_.num_hidden > 0, "must set nhidden correctly" );
            out_.data.shape = mshadow::Shape4( 1, 1, in_.data.shape[1], param_.num_hidden );
        }
        virtual void GetUpdaters( const char *updater, std::vector<IUpdater*> &updaters ){
            updaters.push_back( CreateUpdater( updater, rnd_, wmat_, gwmat_, "wmat" ) );
            if( param_.no_bias != 0 ){
                updaters.push_back( CreateUpdater( updater, rnd_, bias_, gbias_, "bias" ) );
            }
        }
        virtual void SetParam(const char *name, const char* val){
            param_.SetParam( name, val );
        }
        virtual void InitModel(void){
            // resize to correct shape
            wmat_.Resize( mshadow::Shape2( in_.data.shape[0], out_.data.shape[0] ) );
            gwmat_.Resize( wmat_.shape );
            bias_.Resize( mshadow::Shape1( out_.data.shape[0] ) );
            gbias_.Resize( bias_.shape );
            // random initalize
            if (param_.random_type == kGaussian) {
                InitGaussian();
            } else {
                InitUniform();
            }
            bias_ = 0.0f; gwmat_ = 0.0f; gbias_ = 0.0f;
        }
        virtual void InitGaussian(void) {
            rnd_.SampleGaussian( wmat_, 0.0f, param_.init_sigma );
        }
        virtual void InitUniform(void) {
            float a = -sqrt(6.0f / (wmat_.shape[0] + wmat_.shape[1]));
            float b = sqrt(6.0f / (wmat_.shape[0] + wmat_.shape[1]));
            rnd_.SampleUniform( wmat_, a, b);
        }
        virtual void SaveModel(mshadow::utils::IStream &fo) const{
            fo.Write( &param_, sizeof(LayerParam) );
            wmat_.SaveBinary( fo );
            bias_.SaveBinary( fo );
        }
        virtual void LoadModel(mshadow::utils::IStream &fi){
            Assert( fi.Read( &param_, sizeof(LayerParam) ) != 0, "load model");
            wmat_.LoadBinary( fi );
            bias_.LoadBinary( fi );
        }
    private:
        /*! \brief parameters that potentially be useful */
        LayerParam param_;
        /*! \brief random number generator */
        mshadow::Random<xpu> &rnd_;
        /*! \brief input node */
        Node<xpu> &in_;
        /*! \brief output node */
        Node<xpu> &out_;
        /*! \brief weight matrix */
        mshadow::TensorContainer<xpu,2> wmat_;
        /*! \brief bias */
        mshadow::TensorContainer<xpu,1> bias_;
        /*! \brief accumulates the gradient of weight matrix */
        mshadow::TensorContainer<xpu,2> gwmat_;
        /*! \brief accumulates the gradient of bias */
        mshadow::TensorContainer<xpu,1> gbias_;
    };

    /*! \brief softmax layer, do softmax transformation during forward */
    template<typename xpu>
    class SoftmaxLayer: public ILayer{
    public:
        SoftmaxLayer( mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out )
            :out_(out){
            Assert( &in == &out, "softmax layer must self loop e.g layer[1->1] = softmax" );
        }
        virtual void Forward(bool is_train){
            mshadow::Softmax( out_.mat(), out_.mat() );
        }
        virtual void Backprop(bool prop_grad){}
    private:
        /*! \brief only transform on out */
        Node<xpu> &out_;
    };

    template<typename xpu>
    class ConvolutionLayer : public ILayer{
    public:
        ConvolutionLayer( mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out )
            :rnd_(rnd), in_(in), out_(out){
        }
        virtual ~ConvolutionLayer( void ){}
        virtual void Forward(bool is_train) {
            index_t nbatch = in_.data.shape[3];
            for( index_t i = 0; i < nbatch; ++ i ){
                temp_col_ = unpack_patch2col( in_.data[i], param_.kernel_size, param_.stride );
                temp_dst_ = dot( wmat_, temp_col_ );
                out_.data[i] = reshape( temp_dst_, out_.data[i].shape );
            }
            if( param_.no_bias != 0 ){
                // add bias, broadcast bias to dim 2: channel
                out_.data += broadcast<2>( bias_, out_.data.shape );
            }
        }
        virtual void Backprop(bool prop_grad){
            index_t nbatch = in_.data.shape[1];
            real_t scale = 1.0f / nbatch;            
            
            if( param_.no_bias != 0 ){
                gbias_ = scale * sumall_except_dim<2>( out_.data );
            }
            gwmat_ = 0.0f;
            for( index_t i = 0; i < nbatch; ++ i ){
                temp_dst_ = reshape( out_.data[i], temp_dst_.shape );
                temp_col_ = unpack_patch2col( in_.data[i], param_.kernel_size, param_.stride );

                gwmat_ += scale * dot( temp_dst_, temp_col_.T() );
                if( prop_grad ){
                    temp_col_ = dot( temp_dst_.T(), wmat_ );
                    mshadow::PackPatchFromCol( in_.data[i], temp_col_, param_.kernel_size, param_.stride );
                }
            }
        }
        virtual void AdjustNodeShape( void ) {
            const index_t ksize   = static_cast<index_t>( param_.kernel_size );
            const index_t kstride = static_cast<index_t>( param_.stride );
            Assert( param_.num_channel > 0, "must set nchannel correctly" );
            Assert( param_.kernel_size > 0, "must set kernel_size correctly" );
            Assert( ksize <= in_.data.shape[0] && ksize <= in_.data.shape[1], "kernel size exceed input" );
            mshadow::Shape<4> oshape = mshadow::
                Shape4( in_.data.shape[3], param_.num_channel,
                        (in_.data.shape[1] - ksize)/kstride + 1,
                        (in_.data.shape[0] - ksize)/kstride + 1 );
            out_.data.shape = oshape;

            // helper structure
            temp_col_.Resize( mshadow::Shape2( in_.data.shape[2]*ksize*ksize, oshape[1]*oshape[0] ) );
            temp_dst_.Resize( mshadow::Shape2( param_.num_channel, oshape[1]*oshape[0] ) );
        }
        virtual void GetUpdaters( const char *updater, std::vector<IUpdater*> &updaters ){
            updaters.push_back( CreateUpdater( updater, rnd_, wmat_, gwmat_, "wmat" ) );
            if( param_.no_bias != 0 ){
                updaters.push_back( CreateUpdater( updater, rnd_, bias_, gbias_, "bias" ) );
            }
        }
        virtual void SetParam(const char *name, const char* val){
            param_.SetParam( name, val );
        }
        virtual void InitModel(void){
            // resize to correct shape, use 2d to store the weight, since we use dot
            wmat_.Resize( mshadow::Shape2( param_.num_channel, param_.kernel_size*param_.kernel_size ) );
            gwmat_.Resize( wmat_.shape );
            bias_.Resize( mshadow::Shape1( param_.num_channel ) );
            gbias_.Resize( bias_.shape );
            this->InitGaussian();

            bias_ = 0.0f; gwmat_ = 0.0f; gbias_ = 0.0f;
        }
        virtual void InitGaussian(void) {
            rnd_.SampleGaussian( wmat_, 0.0f, param_.init_sigma );
        }
        virtual void SaveModel(mshadow::utils::IStream &fo) const{
            fo.Write( &param_, sizeof(LayerParam) );
            wmat_.SaveBinary( fo );
            bias_.SaveBinary( fo );
        }
        virtual void LoadModel(mshadow::utils::IStream &fi){
            Assert( fi.Read( &param_, sizeof(LayerParam) ) != 0, "load model");
            wmat_.LoadBinary( fi );
            bias_.LoadBinary( fi );
        }
    private:
        /*! \brief parameters that potentially be useful */
        LayerParam param_;
        /*! \brief random number generator */
        mshadow::Random<xpu> &rnd_;
        /*! \brief input node */
        Node<xpu> &in_;
        /*! \brief output node */
        Node<xpu> &out_;
        /*! \brief weight matrix */
        mshadow::TensorContainer<xpu,2> wmat_;
        /*! \brief bias */
        mshadow::TensorContainer<xpu,1> bias_;
        /*! \brief accumulates the gradient of weight matrix */
        mshadow::TensorContainer<xpu,2> gwmat_;
        /*! \brief accumulates the gradient of bias */
        mshadow::TensorContainer<xpu,1> gbias_;
        /*! \brief temporary data structure to store patches */
        mshadow::TensorContainer<xpu,2> temp_col_;
        /*! \brief temporary data structure to store results */
        mshadow::TensorContainer<xpu,2> temp_dst_;
    };

    template<typename xpu,typename ForwardOp, typename BackOp >
    class ActivationLayer : public ILayer{
    public:
        ActivationLayer( Node<xpu> &in, Node<xpu> &out )
            :in_(in), out_(out) {
        }
        virtual ~ActivationLayer( void ){}
        virtual void Forward( bool is_train ) {
            in_.mat() = F<ForwardOp>( in_.mat() );
            mshadow::Copy( out_.mat(), in_.mat() );
        }
        virtual void Backprop( bool prop_grad ){
            in_.mat() = F<BackOp>( in_.mat() ) * out_.mat();
        }
        virtual void AdjustNodeShape( void ) {
            out_.data.shape = in_.data.shape;
        }
    private:
        /*! \brief input node */
        Node<xpu> &in_;
        /*! \brief output node */
        Node<xpu> &out_;
    };
};

namespace cxxnet{
    template<typename xpu>
    class FlattenLayer : public ILayer{
    public:
        FlattenLayer( Node<xpu> &in, Node<xpu> &out )
            :in_(in), out_(out) {
        }
        virtual ~FlattenLayer( void ){}
        virtual void Forward( bool is_train ) {
            out_.data = reshape( in_.data, out_.data.shape );
        }
        virtual void Backprop( bool prop_grad ){
            if( prop_grad ){
                in_.data = reshape( out_.data, in_.data.shape );
            }
        }
        virtual void AdjustNodeShape( void ) {
            mshadow::Shape<4> ishape = in_.data.shape;
            out_.data.shape = mshadow::Shape4( 1, 1, ishape[3], ishape[2]*ishape[1]*ishape[0] );
        }
    private:
        /*! \brief input node */
        Node<xpu> &in_;
        /*! \brief output node */
        Node<xpu> &out_;
    };
};

namespace cxxnet {
    template<typename xpu>
    class DropoutLayer : public ILayer {
    public:
        DropoutLayer(mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out)
            :rnd_(rnd), in_(in), out_(out) {
        }
        virtual void SetParam(const char *name, const char* val){
            param_.SetParam( name, val );
        }
        virtual void Forward( bool is_train ) {
            if (is_train) {
                utils::Assert(param_.dropout_threshold >= 0 && param_.dropout_threshold < 1, "Invalid dropout threshold\n");
                rnd_.SampleUniform(mask_, 0, 1);
                mask_ = F<op::threshold>(mask_, ScalarExp(1 - param_.dropout_threshold));
                in_.mat() = in_.mat() * mask_;
            } else {
                in_.mat() = in_.mat();
            }
            mshadow::Copy( out_.mat(), in_.mat() );
        }
        virtual void Backprop( bool prop_grad ) {
            if (prop_grad) {
                in_.mat() = out_.mat() * mask_;
            }
        }
        virtual void AdjustNodeShape( void ) {
            out_.data.shape = in_.data.shape;
            mask_.Resize(in_.mat().shape);
        }
    private:
        /*! \brief input node */
        Node<xpu> &in_;
        /*! \brief output node */
        Node<xpu> &out_;
        /*! \brief dropout mask */
        mshadow::TensorContainer<xpu, 2> mask_;
        /*! \brief random number generator */
        mshadow::Random<xpu> &rnd_;
        /*! \brief parameters that potentially be useful */
        LayerParam param_;
        /*! \brief scale from caffe, TODO: check other dropout */
        real_t scale_;
    }; // class DropoutLayer
}; // namespace cxxnet

namespace cxxnet{
    template<typename xpu> 
    struct PairTestLayer : public ILayer{
    protected:
        class PairTestUpdater: public IUpdater{
        public:
            PairTestUpdater( IUpdater *umaster, IUpdater *uslave )
                :umaster_(umaster), uslave_(uslave){
                w_mst_.set_pad( false ); w_slv_.set_pad( false );
                g_mst_.set_pad( false ); g_slv_.set_pad( false );
            }
            inline void Sync( void ){
                umaster_->GetData( w_mst_, g_mst_ );
                uslave_->SetData( w_mst_, g_mst_ );
            }
            virtual void Init( void ){
                umaster_->Init(); uslave_->Init();
            }
            virtual void Update( void ){
                umaster_->Update();  uslave_->Update();                
                umaster_->GetData( w_mst_, g_mst_ );
                uslave_->GetData( w_slv_, g_slv_ );
                CmpResult( w_mst_, w_slv_, "update:weight" );
                CmpResult( g_mst_, g_slv_, "update:gradient" );
            }
            virtual void StartRound( int round ) {
                umaster_->StartRound( round );
                uslave_->StartRound( round );
            }
            virtual void SetParam( const char *name, const char *val ) {
                umaster_->SetParam( name, val );
                uslave_->SetParam( name, val );
            }
            
        private:
            IUpdater *umaster_, *uslave_;
            mshadow::TensorContainer<cpu,2> w_mst_, w_slv_;
            mshadow::TensorContainer<cpu,2> g_mst_, g_slv_;            
        };
    public:
        PairTestLayer( mshadow::Random<xpu> &rnd, Node<xpu>&in, Node<xpu>& out,
                       int tmaster, int tslave ):in_(in),out_(out){
            master_ = CreateLayer_( tmaster, rnd, in, out ) ;
            slave_  = CreateLayer_( tslave, rnd, slv_in_, slv_out_ );
        }
        virtual ~PairTestLayer( void ){ 
            delete master_; delete slave_;
            slv_in_.FreeSpace(); slv_out_.FreeSpace();
        }
        virtual void Forward( bool is_train ){
            Copy( slv_in_.data, in_.data );
            master_->Forward( is_train );
            slave_->Forward( is_train );
            CmpResult( out_.data.FlatTo2D(), slv_out_.data.FlatTo2D(), "forward" );
        }
        virtual void Backprop( bool prop_grad ){
            Copy( slv_out_.data, out_.data );
            master_->Backprop( prop_grad );
            slave_->Backprop( prop_grad );
            if( prop_grad ){
                CmpResult( in_.data.FlatTo2D(), slv_in_.data.FlatTo2D(), "backprop" );
            }
        }
    public:
        virtual void AdjustNodeShape( void ){
            slv_in_.data.shape = in_.data.shape;
            master_->AdjustNodeShape();
            slave_->AdjustNodeShape();
            utils::Assert( slv_out_.data.shape == out_.data.shape, "PairTestLayer:AdjustNodeShape mismatch" );
            AllocSpace( slv_in_.data );
            AllocSpace( slv_out_.data );
        }
        virtual void GetUpdaters( const char *updater, std::vector<IUpdater*> &updaters ) {
            std::vector<IUpdater*> umaster, uslave;
            master_->GetUpdaters( updater, umaster );
            slave_->GetUpdaters( updater, uslave );
            utils::Assert( umaster.size() == uslave.size(), "PairTestLayer: number of updaters not match" );
            for( size_t i = 0; i < umaster.size(); ++i ){
                PairTestUpdater *up = new PairTestUpdater( umaster[i], uslave[i] );
                up->Sync(); updaters.push_back( up );
            }
        }
        virtual void SetParam( const char *name, const char* val ) {
            master_->SetParam( name, val );
            slave_->SetParam( name, val );
            if( !strncmp( name, "master:", 7 ) ) master_->SetParam( name+7, val );
            if( !strncmp( name, "slave:", 6 ) ) slave_->SetParam( name+7, val );           
        }
        virtual void InitModel(void) {
            master_->InitModel();
            slave_->InitModel();
        }
        virtual void SaveModel(mshadow::utils::IStream &fo) const {
            master_->SaveModel( fo );
            slave_->SaveModel( fo );
        }
        virtual void LoadModel(mshadow::utils::IStream &fi) {
            master_->LoadModel( fi );
            slave_->LoadModel( fi );
        }
    private:
        template<typename xxpu>
        inline static void CmpResult( mshadow::Tensor<xxpu,2> dmaster, mshadow::Tensor<xxpu,2> dslave, const char *tag ){
            mshadow::TensorContainer<cpu, 2> tmst(false), tslv(false);
            tmst.Resize( dmaster.shape ); tslv.Resize( dslave.shape );
            mshadow::Copy( tmst, dmaster ); mshadow::Copy( tslv, dslave );
            index_t count = tmst.shape.Size();
            double diff = 0.0, ssum = 0.0;
            for( index_t i = 0; i < count; ++i ){
                diff += std::abs( tmst.dptr[i] - tslv.dptr[i] ); 
                ssum += std::abs( tmst.dptr[i] );
            }
            // relative absolute error
            double rerr = diff / ssum;
            if( rerr > 1e-6 ){
                fprintf( stderr, "%s: err=%f\n", tag, rerr );
            }
        }
    private:
        ILayer *master_, *slave_;
        Node<xpu> &in_, &out_;   
        // data that slave takes
        Node<xpu> slv_in_, slv_out_;
    }; 
};

namespace cxxnet{
    /* layer patch that handles memory issues */
    template<typename xpu>
    struct LayerPatch: public ILayer{
    public:
        LayerPatch( ILayer *base, Node<xpu>& in, Node<xpu>& out )
            :base_(base), in_(in), out_(out){}
        virtual ~LayerPatch( void ){ delete base_; }
        virtual void Forward( bool is_train ){
            in_.Pin(); out_.Pin();
            base_->Forward( is_train );
            in_.Unpin(); out_.Unpin();
        }
        virtual void Backprop( bool prop_grad ){
            in_.Pin(); out_.Pin();
            base_->Backprop( prop_grad );
            in_.Unpin(); out_.Unpin();
        }
    public:
        virtual void AdjustNodeShape( void ){
            base_->AdjustNodeShape();
        }
        virtual void GetUpdaters( const char *updater, std::vector<IUpdater*> &updaters ) {
            base_->GetUpdaters( updater, updaters );
        }
        virtual void SetParam( const char *name, const char* val ) {
            base_->SetParam( name, val );
        }
        virtual void InitModel(void) {
            base_->InitModel();
        }
        virtual void SaveModel(mshadow::utils::IStream &fo) const {
            base_->SaveModel( fo );
        }
        virtual void LoadModel(mshadow::utils::IStream &fi) {
            base_->LoadModel( fi );
        }
    private:
        ILayer *base_;
        Node<xpu> &in_, &out_;
    };
};

namespace cxxnet{
    inline int GetLayerType( const char *type ){
        if( !strncmp( type, "pairtest-", 9 ) ){
            char tmaster[256], tslave[256];
            sscanf( type + 9, "%[^-]-%[^:]", tmaster, tslave );
            return 10000 + GetLayerType(tmaster) * 100  + GetLayerType(tslave);
        }
        using namespace layer_type;
        if( !strcmp( type, "fullc") )   return kFullConnect;
        if( !strcmp( type, "softmax") ) return kSoftmax;
        if( !strcmp( type, "relu") ) return kRectifiedLinear;
        if( !strcmp( type, "sigmoid") ) return kSigmoid;
        if( !strcmp( type, "tanh") ) return kTanh;
        if( !strcmp( type, "softplus") ) return kSoftplus;
        if( !strcmp( type, "flatten") )  return kFlatten;
        if( !strcmp( type, "dropout") ) return kDropout;
        if( !strcmp( type, "dropconn") ) return kDropConn;
        if( !strcmp( type, "conv") )     return kConv;
        if( !strcmp( type, "caffe") ) return kCaffe;
        Error("unknown layer type" );
        return 0;
    }

    template<typename xpu>
    inline ILayer* CreateLayer_( int type, mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out ){
        using namespace layer_type;
        if( type >= 10000 ){
            return new PairTestLayer<xpu>( rnd, in, out, (type/100)%100, type%100 );
        } 
        switch( type ){
        case kFullConnect: return new FullConnectLayer<xpu>( rnd, in, out );
        case kSoftmax    : return new SoftmaxLayer<xpu>( rnd, in, out );
        case kSigmoid : return new ActivationLayer<xpu,op::sigmoid,op::sigmoid_grad>(in, out);
        case kTanh    : return new ActivationLayer<xpu,op::tanh,op::tanh_grad>(in, out);
        case kRectifiedLinear: return new ActivationLayer<xpu,op::relu,op::relu_grad>(in, out);
        case kSoftplus: return new ActivationLayer<xpu,op::softplus,op::softplus_grad>(in, out);
        case kFlatten:  return new FlattenLayer<xpu>( in, out );
        // TODO:
        case kDropout: return new DropoutLayer<xpu>(rnd, in, out);
        case kConv:    return new ConvolutionLayer<xpu>( rnd, in, out );
#if CXXNET_ADAPT_CAFFE
        case kCaffe: return new CaffeLayer<xpu>(rnd,in,out);
#endif
        default: Error("unknown layer type");
        }
        return NULL;
    }
    template<typename xpu>
    inline ILayer* CreateLayer( int type, mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out ){
        return new LayerPatch<xpu>( CreateLayer_<xpu>(type,rnd,in,out), in, out );
    }
    template<typename xpu>
    inline ILayer* CreateLayer( const char *type, mshadow::Random<xpu> &rnd, Node<xpu> &in, Node<xpu> &out ){
        return CreateLayer( GetLayerType(type), rnd, in, out );
    }
};


#endif // CXXNET_LAYER_INL_HPP
